/*
 * krunker_cheat_internal.h - the eval backend behind krunker_cheat_answer().
 * Web: an EM_JS indirect eval in the page (krunker_cheat.c). Native: the
 * QuickJS sandbox (krunker_cheat_qjs.c). Client-only.
 */
#ifndef KRUNKER_CHEAT_INTERNAL_H
#define KRUNKER_CHEAT_INTERNAL_H

#include <stddef.h>
#include <stdint.h>

/*
 * The game handler's steps 4-7 on the decrypted plaintext bytes (any bytes,
 * may contain NUL):
 *   src = JSON.parse(new TextDecoder().decode(plain))
 *   R   = (0, eval)(src)       (throw -> {r:null,e:{_eval: e.message || "eval_error"}})
 *   out = JSON.stringify(R)
 * Returns 0 with *json = malloc'd UTF-8 String(out), or *json = NULL when out
 * is undefined. Returns non-zero when the real handler would throw out to its
 * outer try/catch and send no ccr (JSON.parse/stringify throws, `e.message` on
 * a null/undefined throw value, the sandbox interrupts or runs out of memory
 * outside the payload's own try), or the engine cannot be created.
 */
int krunker_cheat_eval(const uint8_t *plain, size_t len, char **json);

#ifndef __EMSCRIPTEN__
/* Native sandbox limits (krunker_cheat_qjs.c), one place to tune them. */
#define KRUNKER_CHEAT_QJS_MEMORY_LIMIT (32u * 1024u * 1024u) /* bytes per challenge runtime */
#define KRUNKER_CHEAT_QJS_STACK_LIMIT (256u * 1024u)         /* bytes of C stack for JS */
#define KRUNKER_CHEAT_QJS_TIME_LIMIT_MS 250                   /* wall clock per challenge */

/* Test hook: run raw JS `src` (skipping the TextDecoder + JSON.parse step) in
 * a fresh sandbox; same result contract as krunker_cheat_eval. */
int krunker_cheat_eval_source(const char *src, size_t len, char **json);

/* WHATWG UTF-8 decode (TextDecoder default: BOM stripped, maximal-subpart
 * U+FFFD replacement) re-encoded as well-formed UTF-8. `out` needs 3*len
 * bytes; returns the output length. */
size_t krunker_cheat_utf8_sanitize(const uint8_t *in, size_t len, char *out);
#endif

#endif /* KRUNKER_CHEAT_INTERNAL_H */
