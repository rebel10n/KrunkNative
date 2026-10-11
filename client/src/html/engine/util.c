/* util.c - see util.h */
#include "util.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>

/* ------------------------------------------------------------------------- */
/* strings                                                                    */
/* ------------------------------------------------------------------------- */

int kn_str_ieq(const char *s, size_t len, const char *lit)
{
    size_t i;
    for (i = 0; i < len; i++) {
        if (lit[i] == '\0' || kn_to_lower((unsigned char)s[i]) != kn_to_lower((unsigned char)lit[i]))
            return 0;
    }
    return lit[i] == '\0';
}

int kn_str_eq(const char *s, size_t len, const char *lit)
{
    return strlen(lit) == len && memcmp(s, lit, len) == 0;
}

const char *kn_str_trim(const char *s, size_t *len)
{
    size_t n = *len;
    while (n > 0 && kn_is_space((unsigned char)s[0])) { s++; n--; }
    while (n > 0 && kn_is_space((unsigned char)s[n - 1])) n--;
    *len = n;
    return s;
}

uint64_t kn_hash_str_ci(const char *s, size_t len)
{
    uint64_t h = KN_FNV64_INIT;
    for (size_t i = 0; i < len; i++) {
        h ^= (unsigned char)kn_to_lower((unsigned char)s[i]);
        h *= 0x100000001b3ULL;
    }
    return h;
}

/* ------------------------------------------------------------------------- */
/* atoms: open-addressing hash set of interned strings                        */
/* ------------------------------------------------------------------------- */

typedef struct AtomEntry {
    const char *str;
    size_t len;
    uint64_t hash;
} AtomEntry;

static AtomEntry *g_atoms;
static size_t g_atom_cap;
static size_t g_atom_count;
static KnArena g_atom_arena;

static uint64_t atom_hash(const char *s, size_t len)
{
    return kn_hash_bytes(KN_FNV64_INIT, s, len) | 1; /* never 0: 0 == empty slot */
}

static void atom_rehash(size_t new_cap)
{
    AtomEntry *old = g_atoms;
    size_t old_cap = g_atom_cap;
    g_atoms = calloc(new_cap, sizeof(*g_atoms));
    if (!g_atoms) abort();
    g_atom_cap = new_cap;
    for (size_t i = 0; i < old_cap; i++) {
        if (!old[i].hash) continue;
        size_t j = old[i].hash & (new_cap - 1);
        while (g_atoms[j].hash) j = (j + 1) & (new_cap - 1);
        g_atoms[j] = old[i];
    }
    free(old);
}

const char *kn_atom(const char *s, size_t len)
{
    if (g_atom_cap == 0) {
        atom_rehash(1024);
        kn_arena_init(&g_atom_arena, 64 * 1024);
    }
    uint64_t h = atom_hash(s, len);
    size_t i = h & (g_atom_cap - 1);
    while (g_atoms[i].hash) {
        if (g_atoms[i].hash == h && g_atoms[i].len == len && memcmp(g_atoms[i].str, s, len) == 0)
            return g_atoms[i].str;
        i = (i + 1) & (g_atom_cap - 1);
    }
    char *copy = kn_arena_strdup(&g_atom_arena, s, len);
    g_atoms[i].str = copy;
    g_atoms[i].len = len;
    g_atoms[i].hash = h;
    g_atom_count++;
    if (g_atom_count * 2 > g_atom_cap) atom_rehash(g_atom_cap * 2);
    return copy;
}

const char *kn_atom_lower(const char *s, size_t len)
{
    char stack[256] = { 0 };
    char *buf = len <= sizeof(stack) ? stack : malloc(len);
    if (!buf) abort();
    for (size_t i = 0; i < len; i++) buf[i] = (char)kn_to_lower((unsigned char)s[i]);
    const char *a = kn_atom(buf, len);
    if (buf != stack) free(buf);
    return a;
}

/* ------------------------------------------------------------------------- */
/* arena                                                                      */
/* ------------------------------------------------------------------------- */

struct KnArenaBlock {
    KnArenaBlock *next;
    size_t used, cap;
    /* data follows (16-byte aligned) */
};

#define ARENA_HDR ((sizeof(KnArenaBlock) + 15) & ~(size_t)15)

void kn_arena_init(KnArena *a, size_t block_size)
{
    a->head = NULL;
    a->block_size = block_size ? block_size : 16 * 1024;
}

void *kn_arena_alloc(KnArena *a, size_t size)
{
    size = (size + 15) & ~(size_t)15;
    KnArenaBlock *b = a->head;
    if (!b || b->used + size > b->cap) {
        size_t cap = size > a->block_size ? size : a->block_size;
        b = malloc(ARENA_HDR + cap);
        if (!b) abort();
        b->next = a->head;
        b->used = 0;
        b->cap = cap;
        a->head = b;
    }
    char *p = (char *)b + ARENA_HDR + b->used;
    b->used += size;
    memset(p, 0, size);
    return p;
}

char *kn_arena_strdup(KnArena *a, const char *s, size_t len)
{
    char *p = kn_arena_alloc(a, len + 1);
    memcpy(p, s, len);
    p[len] = '\0';
    return p;
}

void kn_arena_reset(KnArena *a)
{
    for (KnArenaBlock *b = a->head; b; b = b->next) b->used = 0;
}

void kn_arena_free(KnArena *a)
{
    KnArenaBlock *b = a->head;
    while (b) {
        KnArenaBlock *next = b->next;
        free(b);
        b = next;
    }
    a->head = NULL;
}

/* ------------------------------------------------------------------------- */
/* vec / string builder                                                       */
/* ------------------------------------------------------------------------- */

void *kn_vec_grow(void *data, size_t *cap, size_t need, size_t elem_size)
{
    size_t c = *cap ? *cap : 8;
    while (c < need) c *= 2;
    void *p = realloc(data, c * elem_size);
    if (!p) abort();
    *cap = c;
    return p;
}

void kn_sb_append(KnStrBuf *sb, const char *s, size_t len)
{
    if (sb->len + len + 1 > sb->cap)
        sb->data = kn_vec_grow(sb->data, &sb->cap, sb->len + len + 1, 1);
    memcpy(sb->data + sb->len, s, len);
    sb->len += len;
    sb->data[sb->len] = '\0';
}

void kn_sb_append_cstr(KnStrBuf *sb, const char *s) { kn_sb_append(sb, s, strlen(s)); }
void kn_sb_append_char(KnStrBuf *sb, char c) { kn_sb_append(sb, &c, 1); }

void kn_sb_appendf(KnStrBuf *sb, const char *fmt, ...)
{
    char tmp[512];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(tmp, sizeof(tmp), fmt, ap);
    va_end(ap);
    if (n < 0) return;
    if ((size_t)n < sizeof(tmp)) {
        kn_sb_append(sb, tmp, (size_t)n);
        return;
    }
    char *big = malloc((size_t)n + 1);
    if (!big) abort();
    va_start(ap, fmt);
    vsnprintf(big, (size_t)n + 1, fmt, ap);
    va_end(ap);
    kn_sb_append(sb, big, (size_t)n);
    free(big);
}

void kn_sb_clear(KnStrBuf *sb)
{
    sb->len = 0;
    if (sb->data) sb->data[0] = '\0';
}

void kn_sb_free(KnStrBuf *sb)
{
    free(sb->data);
    sb->data = NULL;
    sb->len = sb->cap = 0;
}

/* ------------------------------------------------------------------------- */
/* utf8                                                                       */
/* ------------------------------------------------------------------------- */

int kn_utf8_decode(const char *s, size_t len, uint32_t *cp)
{
    const unsigned char *p = (const unsigned char *)s;
    if (len == 0) { *cp = 0xFFFD; return 0; }
    unsigned char c = p[0];
    int n;
    uint32_t v;
    if (c < 0x80) { *cp = c; return 1; }
    else if ((c & 0xE0) == 0xC0) { n = 2; v = c & 0x1F; }
    else if ((c & 0xF0) == 0xE0) { n = 3; v = c & 0x0F; }
    else if ((c & 0xF8) == 0xF0) { n = 4; v = c & 0x07; }
    else { *cp = 0xFFFD; return 1; }
    if ((size_t)n > len) { *cp = 0xFFFD; return 1; }
    for (int i = 1; i < n; i++) {
        if ((p[i] & 0xC0) != 0x80) { *cp = 0xFFFD; return 1; }
        v = (v << 6) | (p[i] & 0x3F);
    }
    if ((n == 2 && v < 0x80) || (n == 3 && v < 0x800) || (n == 4 && (v < 0x10000 || v > 0x10FFFF)) ||
        (v >= 0xD800 && v <= 0xDFFF)) {
        *cp = 0xFFFD;
        return 1;
    }
    *cp = v;
    return n;
}

int kn_utf8_encode(uint32_t cp, char *out)
{
    if (cp < 0x80) { out[0] = (char)cp; return 1; }
    if (cp < 0x800) {
        out[0] = (char)(0xC0 | (cp >> 6));
        out[1] = (char)(0x80 | (cp & 0x3F));
        return 2;
    }
    if (cp < 0x10000) {
        out[0] = (char)(0xE0 | (cp >> 12));
        out[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
        out[2] = (char)(0x80 | (cp & 0x3F));
        return 3;
    }
    out[0] = (char)(0xF0 | (cp >> 18));
    out[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
    out[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
    out[3] = (char)(0x80 | (cp & 0x3F));
    return 4;
}
