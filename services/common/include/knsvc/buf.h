/*
 * knsvc/buf.h - growable byte buffer used by every knsvc module.
 *
 * A kn_buf owns `data` (heap, always NUL-terminated past `len` so text can be
 * used as a C string). Zero-initialise ({0}) or call kn_buf_init(); release with
 * kn_buf_free(). Appends return 0 on success, -1 on allocation failure.
 */
#ifndef KNSVC_BUF_H
#define KNSVC_BUF_H

#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __GNUC__
#define KN_PRINTF(fmt_idx, arg_idx) __attribute__((format(printf, fmt_idx, arg_idx)))
#else
#define KN_PRINTF(fmt_idx, arg_idx)
#endif

typedef struct kn_buf {
    char *data;  /* NULL until the first append */
    size_t len;  /* bytes in use */
    size_t cap;  /* allocated bytes (excluding the NUL guard) */
} kn_buf;

void kn_buf_init(kn_buf *b);
void kn_buf_free(kn_buf *b);
void kn_buf_reset(kn_buf *b);                       /* len = 0, keep memory */
int kn_buf_reserve(kn_buf *b, size_t extra);        /* ensure cap >= len + extra */
int kn_buf_append(kn_buf *b, const void *p, size_t n);
int kn_buf_append_str(kn_buf *b, const char *s);
int kn_buf_append_byte(kn_buf *b, uint8_t c);
int kn_buf_appendf(kn_buf *b, const char *fmt, ...) KN_PRINTF(2, 3);
int kn_buf_vappendf(kn_buf *b, const char *fmt, va_list ap);
void kn_buf_consume(kn_buf *b, size_t n);           /* drop n bytes from the front */
char *kn_buf_detach(kn_buf *b, size_t *len_out);    /* hand ownership of data to caller (free()) */

#endif
