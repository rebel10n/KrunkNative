// KrunkScript code window, language side (GL- and DOM-free: editor_serialize_test links it).
//
// v7.1.4's script window is scripting.html, which the reference tree lacks. The live page
// (krunker.io/scripting.html + css/scripting.css, Wayback Machine snapshots 2021-10 .. 2024-03,
// identical apart from build ids; tools/editor/fetch_scripting_ref.sh) is CodeMirror 5.61.1 with:
//   mode "krunkscript" = CodeMirror.defineSimpleMode over these rules, in order (start state):
//     comment  /\s*#[^\n\r]*/
//     keyword  /\b(public|static|action|break|continue|return|if|else|for|in|while|addTo|remove|
//               lengthOf|notEmpty|hasProp|toStr|toNum)\b/
//     def,null /\b([_a-zA-Z][_a-zA-Z0-9]*)(\()/        (an identifier followed by "(")
//     type     /\b(bool|num|str|obj)\b/
//     atom     /\b(GAME|true|false)\b/                 (initCodeMirror(["GAME"], ...))
//     variable /\b[_a-zA-Z][_a-zA-Z0-9]*\b/
//     number   /\d+\.?\d*/
//     string   /("[^"\n\r]*")|('[^'\n\r]*')/
//     null     /[\{\[\(]/ (indent)   /[\}\]\)]/ (dedent)
//     meta: lineComment "#", fold "brace"
//   options: lineNumbers, matchBrackets, indentWithTabs, tabSize 4, indentUnit 4,
//     styleActiveLine, autoCloseBrackets, foldGutter, theme material-palenight; keys Ctrl-/
//     toggleComment, Ctrl-Space hint, Ctrl-S KE.updateScript(), Alt-F find, Ctrl-Q fold.
//   hint (show-hint, completeSingle false), on every cursorActivity: the [\w.]+ word before the
//     cursor; nothing when "word" / 'word' occurs in the line; else the KSS autoComplete entries
//     whose text starts with it (case-insensitive), or that do after an alias (UTILS, Math)
//     replaces its expansion; at most 500, rendered "text <a>type</a>".
// The colours are the theme's (editor_script.c); this file is the tokenizer, the hint list,
// the bracket / indentation / comment helpers and the script store, all byte-offset based.
#pragma once
#include <cJSON.h>
#include <stddef.h>

// CodeMirror token styles of the krunkscript mode (EKS_NONE = untokenized text)
enum { EKS_NONE, EKS_COMMENT, EKS_KEYWORD, EKS_DEF, EKS_TYPE, EKS_ATOM, EKS_VARIABLE, EKS_NUMBER, EKS_STRING, EKS__COUNT };
const char *ed_ks_style_name(int style);    // "comment" ... ("" for EKS_NONE)

typedef struct {
    int start, end;   // byte offsets into the text
    int style;        // EKS_*
} EdKsToken;

// Tokens of the whole text (styled tokens only, in order). Returns the count, which may exceed
// max (then only max were written).
int ed_ks_tokenize(const char *src, size_t len, EdKsToken *out, int max);
// Style at byte offset `off` (EKS_NONE outside tokens). tokens from ed_ks_tokenize.
int ed_ks_style_at(const EdKsToken *toks, int n, int off);

// matchBrackets: the bracket before the caret, else after it, and its partner (brackets inside
// strings / comments are skipped; the scan stops after 10000 chars like CodeMirror's
// maxScanLen). 1 = matched (*a, *b), -1 = unmatched (*a only), 0 = no bracket at the caret.
int ed_ks_match_bracket(const char *src, size_t len, const EdKsToken *toks, int n, int caret, int *a, int *b);

// ---- autocomplete
#define ED_KS_HINT_MAX 500
typedef struct {
    char text[192];   // what is inserted (alias-substituted)
    char type[48];
} EdKsHint;
// The hint list for the cursor at byte `col` of `line` (one line, no newline): *from = start
// of the replaced word. Returns the entry count (<= max). entries = KSS autoComplete
// [{text,type}], alias = {"UTILS":"GAME.UTILS",...}.
int ed_ks_hints(const char *line, size_t len, int col, const cJSON *entries, const cJSON *alias, EdKsHint *out, int max, int *from);

// ---- editing helpers (CodeMirror indentWithTabs / tabSize 4 / indentUnit 4)
#define ED_KS_TAB_SIZE 4
// Indentation (columns) the simple mode gives a new line at byte `pos` (the mode's indent
// stack over the text before pos; text_after = the rest of that line, for dedents).
int ed_ks_indent_at(const char *src, size_t len, int pos);
// Whitespace for `cols` columns with tabs ("\t" per 4 columns, then spaces). Returns length.
int ed_ks_indent_string(int cols, char *out, size_t n);
// Leading-whitespace width (columns) and byte length of the line starting at line_start.
int ed_ks_line_indent(const char *src, size_t len, int line_start, int *bytes);
// closebrackets: what typing `ch` does with the caret at `pos` (empty selection):
// 0 plain insert, 1 insert the pair, 2 skip over the existing closer.
int ed_ks_close_action(const char *src, size_t len, int pos, char ch);
char ed_ks_closer(char open);                 // ( [ { ' " -> its closer, else 0
// toggleComment over the lines touching [a, b): a malloc'd replacement for the line range
// [*ra, *rb). NULL = nothing to do.
char *ed_ks_toggle_comment(const char *src, size_t len, int a, int b, int *ra, int *rb);

// ---- script store: EditorProject.scripts {"client": code, "server": code} ("" = template)
// side: 0 client, 1 server, 2 shared (the tabs in order)
const char *ed_ks_script_text(const cJSON *scripts, int side); // the code, or the template when empty
void ed_ks_script_store(cJSON *scripts, int side, const char *code);

// ---- diagnostics (shared/include/ks.h ks_validate): the first error, then warnings
typedef struct {
    int line, col;    // 1-based (line 0: not tied to a position)
    int error;        // 1 error, 0 warning
    char message[256];
} EdKsDiag;
int ed_ks_check(const char *code, int side, EdKsDiag *out, int max); // count written (<= max)
