# Vendored Lexbor

[Lexbor](https://github.com/lexbor/lexbor) HTML5 parser + DOM, CSS parser and
selectors engine, amalgamated into one translation unit for the in-house HTML/CSS UI
engine (`client/src/html/engine`, `doc/html-engine-plan.md`). Lexbor is used only
through `dom.c` / `css_sheet.c` in the engine.

| | |
|---|---|
| Upstream | https://github.com/lexbor/lexbor (branch `master`) |
| Commit | `f4cbbcd91359a0ec9499e3ce7e263de629482d61` (2026-09-28, "Core: fixed allocation and move sizes in lexbor_array_obj_t. (#428)") |
| Version | 3.1.0 (`version` file; modules css-1.4.0, html-2.10.0, selectors-0.6.0; deps core-3.1.0, dom-2.1.0, ns-1.4.0, tag-1.5.0) |
| Port | `posix` (no threads; builds with GCC, Clang, MinGW and emcc) |
| License | Apache-2.0, see `LICENSE` (upstream LICENSE + NOTICE) |

## Files

- `lexbor.h` — declarations: includes, every public header of the selected modules,
  `lxb_inline` helpers and the `static const` callback tables upstream also keeps in
  headers. Guarded by `KRUNKNATIVE_LEXBOR_H`; defines `LEXBOR_STATIC` if unset.
- `lexbor.c` — implementation: the feature-test defines (`_POSIX_C_SOURCE`, ...),
  `<dirent.h>` (only the fs port needs it), then upstream's Resources (lookup tables)
  and Source Code sections. One ~2.8 MB TU, ~7 s with `gcc -O2`.
- `CMakeLists.txt` — `add_library(lexbor STATIC)`, public include dir, public
  `LEXBOR_STATIC`, warnings off on this target only.
- `regenerate.sh` — produces the three files above from upstream.

Static library size (x86_64): `gcc -O2` 1.86 MB, `gcc -g -O0` 2.88 MB, clang Debug
(cmake-build-debug) 3.09 MB, `emcc -O2` 1.39-1.49 MB.

## Regenerating

```sh
client/lib/lexbor/regenerate.sh                 # clones the pinned commit into a temp dir
client/lib/lexbor/regenerate.sh /path/to/lexbor # reuse an existing checkout (it is checked out to the pin)
```

Equivalent by hand, from a checkout of the pinned commit:

```sh
perl single.pl --port=posix html css selectors > lexbor-single.h
```

and split the resulting file at its `Defines:` / `Includes:` / `Headers:` /
`Resources:` / `Source Code:` section comments as described above (the script does
exactly that with awk). To move the pin, change `LEXBOR_COMMIT` in `regenerate.sh`,
update the table above, rerun the script and `make ui-test ARGS=lexbor-probe`.

Do not edit `lexbor.h` / `lexbor.c` by hand.
