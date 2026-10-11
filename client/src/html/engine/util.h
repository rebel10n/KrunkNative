/*
 * util.h - small shared helpers for the HTML/CSS engine: arena allocator,
 * growable arrays, interned strings (atoms), UTF-8, string builder, hashing
 * and ASCII helpers.  Implementation in util.c.  C11, no dependencies.
 */
#ifndef KN_HTML_UTIL_H
#define KN_HTML_UTIL_H

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------------- */
/* ASCII                                                                      */
/* ------------------------------------------------------------------------- */

static inline int kn_is_space(int c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f'; }
static inline int kn_is_digit(int c) { return c >= '0' && c <= '9'; }
static inline int kn_is_hex(int c) { return kn_is_digit(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'); }
static inline int kn_is_alpha(int c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); }
static inline int kn_to_lower(int c) { return (c >= 'A' && c <= 'Z') ? c + 32 : c; }
static inline int kn_hex_val(int c) { return kn_is_digit(c) ? c - '0' : (kn_to_lower(c) - 'a' + 10); }

/* Case-insensitive compare of a (ptr,len) slice against a NUL-terminated
 * ASCII literal.  Returns 1 when equal. */
int kn_str_ieq(const char *s, size_t len, const char *lit);
/* Case-sensitive variant. */
int kn_str_eq(const char *s, size_t len, const char *lit);
/* Trim ASCII whitespace from both ends; returns the new start, updates *len. */
const char *kn_str_trim(const char *s, size_t *len);

/* ------------------------------------------------------------------------- */
/* Hashing (FNV-1a 64)                                                        */
/* ------------------------------------------------------------------------- */

#define KN_FNV64_INIT 0xcbf29ce484222325ULL

static inline uint64_t kn_hash_bytes(uint64_t h, const void *data, size_t len)
{
    const unsigned char *p = (const unsigned char *)data;
    for (size_t i = 0; i < len; i++) {
        h ^= p[i];
        h *= 0x100000001b3ULL;
    }
    return h;
}

/* Case-insensitive hash of an ASCII string slice (used by lookup tables). */
uint64_t kn_hash_str_ci(const char *s, size_t len);

/* ------------------------------------------------------------------------- */
/* Atoms (interned, immutable, process-lifetime strings)                      */
/* ------------------------------------------------------------------------- */

/* Returns a stable NUL-terminated copy; equal contents => identical pointer.
 * Never freed.  Safe to compare atoms with ==. */
const char *kn_atom(const char *s, size_t len);
/* Same, but the stored copy is ASCII lower-cased first. */
const char *kn_atom_lower(const char *s, size_t len);
static inline const char *kn_atom_cstr(const char *s) { return kn_atom(s, strlen(s)); }

/* ------------------------------------------------------------------------- */
/* Arena                                                                      */
/* ------------------------------------------------------------------------- */

typedef struct KnArenaBlock KnArenaBlock;

typedef struct KnArena {
    KnArenaBlock *head;
    size_t block_size;
} KnArena;

void  kn_arena_init(KnArena *a, size_t block_size);
void *kn_arena_alloc(KnArena *a, size_t size);        /* 16-byte aligned, zeroed */
char *kn_arena_strdup(KnArena *a, const char *s, size_t len);
void  kn_arena_reset(KnArena *a);                     /* keep blocks, rewind */
void  kn_arena_free(KnArena *a);

/* ------------------------------------------------------------------------- */
/* Growable array (type-generic macros over a header struct)                  */
/* ------------------------------------------------------------------------- */
/*
 *   typedef struct { int *data; size_t len, cap; } IntVec;
 *   IntVec v = {0};
 *   KN_VEC_PUSH(&v, 42);
 *   KN_VEC_FREE(&v);
 */
void *kn_vec_grow(void *data, size_t *cap, size_t need, size_t elem_size);

#define KN_VEC_RESERVE(v, n) \
    ((void)(((v)->len + (n) > (v)->cap) ? \
        ((v)->data = kn_vec_grow((v)->data, &(v)->cap, (v)->len + (n), sizeof(*(v)->data))) : 0))
#define KN_VEC_PUSH(v, item) \
    (KN_VEC_RESERVE(v, 1), (v)->data[(v)->len++] = (item))
#define KN_VEC_FREE(v) \
    (free((v)->data), (v)->data = NULL, (v)->len = (v)->cap = 0)

/* ------------------------------------------------------------------------- */
/* String builder                                                             */
/* ------------------------------------------------------------------------- */

typedef struct KnStrBuf {
    char *data;            /* always NUL-terminated once anything was appended */
    size_t len, cap;
} KnStrBuf;

void kn_sb_append(KnStrBuf *sb, const char *s, size_t len);
void kn_sb_append_cstr(KnStrBuf *sb, const char *s);
void kn_sb_append_char(KnStrBuf *sb, char c);
void kn_sb_appendf(KnStrBuf *sb, const char *fmt, ...);
void kn_sb_clear(KnStrBuf *sb);
void kn_sb_free(KnStrBuf *sb);

/* ------------------------------------------------------------------------- */
/* UTF-8                                                                      */
/* ------------------------------------------------------------------------- */

/* Decodes one code point; returns bytes consumed (>= 1).  Invalid sequences
 * yield U+FFFD and consume 1 byte. */
int kn_utf8_decode(const char *s, size_t len, uint32_t *cp);
/* Encodes cp into out (at least 4 bytes); returns bytes written. */
int kn_utf8_encode(uint32_t cp, char *out);

#ifdef __cplusplus
}
#endif

#endif /* KN_HTML_UTIL_H */
