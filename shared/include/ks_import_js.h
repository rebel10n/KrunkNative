// KrunkScript importer for compiled-JS map scripts (doc/v7/krunkscript.md WP7, 1.16).
//
// Two published v7.1.4 maps (SniperRoyale2, Verb_Arena) ship their client script as the
// official compiler's JavaScript output instead of KrunkScript source. KrunkNative has no
// JavaScript engine: this importer recognises that lowering (the fixed prelude,
// `function V_x(..){try{LC=0;...}catch(e){EH(e.message)}}`, `this.GAME.V_A.V_b.bind(this)(..)`,
// the BC/A/S/B/O/F/L/NE/HP/FC/DE helpers, loop guards, `let V_x`, `===`/`!==`, the module
// epilogue) and translates it back to KrunkScript source for the normal front end
// (ks_compile / ks_validate). Anything outside that shape fails with a diagnostic; there is
// no general JavaScript support.
//
// Shared code: no dependencies beyond libc and the ks front-end tables.
#ifndef KS_IMPORT_JS_H
#define KS_IMPORT_JS_H

#include <stddef.h>

#include "ks.h"

#ifdef __cplusplus
extern "C" {
#endif

// Translate compiled JS (`ks_is_compiled_js(js, len)`) to KrunkScript source. `side` types
// the hook parameters (4.1). Returns a malloc'd NUL-terminated buffer (caller frees) and its
// length in *out_len, or NULL with *err filled (line/col in the JS text; err may be NULL).
// Statements keep the original KrunkScript line numbers where the lowering records them
// (BC / RE / FC line arguments), so runtime errors point at the author's lines.
char *ks_import_js(const char *js, size_t len, KsSide side, size_t *out_len, KsDiag *err);

// Logged by the game hosts after a successful import (ks_game_load).
#define KS_IMPORT_JS_MESSAGE "KrunkScript: running the compiled JavaScript script through the importer"

#ifdef __cplusplus
}
#endif

#endif
