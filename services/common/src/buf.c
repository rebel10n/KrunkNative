/* knsvc/buf.h implementation. */
#include "knsvc/buf.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void kn_buf_init(kn_buf *b) {
    b->data = NULL;
    b->len = 0;
    b->cap = 0;
}

void kn_buf_free(kn_buf *b) {
    free(b->data);
    kn_buf_init(b);
}

void kn_buf_reset(kn_buf *b) {
    b->len = 0;
    if (b->data) b->data[0] = 0;
}

int kn_buf_reserve(kn_buf *b, size_t extra) {
    if (extra > (size_t)-1 / 2 - b->len) return -1;
    size_t need = b->len + extra;
    if (b->data && need <= b->cap) return 0;
    size_t cap = b->cap ? b->cap : 64;
    while (cap < need) cap *= 2;
    char *p = realloc(b->data, cap + 1);
    if (!p) return -1;
    if (!b->data) p[0] = 0;
    b->data = p;
    b->cap = cap;
    return 0;
}

int kn_buf_append(kn_buf *b, const void *p, size_t n) {
    if (kn_buf_reserve(b, n) != 0) return -1;
    if (n) memcpy(b->data + b->len, p, n);
    b->len += n;
    b->data[b->len] = 0;
    return 0;
}

int kn_buf_append_str(kn_buf *b, const char *s) {
    return kn_buf_append(b, s, strlen(s));
}

int kn_buf_append_byte(kn_buf *b, uint8_t c) {
    return kn_buf_append(b, &c, 1);
}

int kn_buf_vappendf(kn_buf *b, const char *fmt, va_list ap) {
    va_list cp;
    va_copy(cp, ap);
    int n = vsnprintf(NULL, 0, fmt, cp);
    va_end(cp);
    if (n < 0 || kn_buf_reserve(b, (size_t)n) != 0) return -1;
    vsnprintf(b->data + b->len, (size_t)n + 1, fmt, ap);
    b->len += (size_t)n;
    return 0;
}

int kn_buf_appendf(kn_buf *b, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int r = kn_buf_vappendf(b, fmt, ap);
    va_end(ap);
    return r;
}

void kn_buf_consume(kn_buf *b, size_t n) {
    if (n >= b->len) {
        kn_buf_reset(b);
        return;
    }
    memmove(b->data, b->data + n, b->len - n);
    b->len -= n;
    b->data[b->len] = 0;
}

char *kn_buf_detach(kn_buf *b, size_t *len_out) {
    if (!b->data && kn_buf_reserve(b, 0) != 0) return NULL;
    char *p = b->data;
    if (len_out) *len_out = b->len;
    kn_buf_init(b);
    return p;
}
