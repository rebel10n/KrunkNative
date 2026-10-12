# KrunkScript (Krunker v7.1.4) — implementable specification for KrunkNative

Status: spec, implemented by the work packages of 6.8 (table below). Target: Krunker **v7.1.4**,
ported **in place**.
There is no v3.1.1 compatibility mode or runtime toggle; v3.1.1 is preserved on the `v3.1.1`
branch/tag. v3.1.1 map JSON must still load: a map without a `scripts` field simply runs no script.

| WP | Status | Where |
|---|---|---|
| WP1 front end | landed | `shared/src/ks/ks_lex.c`, `ks_parse.c`, `ks_check.c`, `ks_api_table.c`, `ksc check`, `ks_front_test` |
| WP2 runtime | landed | `shared/src/ks/ks_compile.c`, `ks_vm.c`, `ks_value.c`, `ks_number.c`, `ks_serial.c`, `ks_stdlib.c`, `ksc run`, `ks_vm_test` |
| WP3 server host | landed | `shared/src/ks_game/`, `server/src/ks_server_host.c`, `ks_server_test`, `server/tests/ks_e2e.py` |
| WP4 client host | landed | `client/src/ks/` |
| WP5 overlay canvas | landed | `client/src/ks/canvas2d.c`, `ks_overlay*.c`, `canvas2d_test` |
| WP6 persistence | landed | `server/src/ks_storage.c`, `ks_storage_test`, `server/tests/ks_storage_e2e.py` |
| WP7 compiled-JS importer | landed: `SniperRoyale2` and `Verb_Arena` run | `shared/src/ks/ks_import_js.c` (`shared/include/ks_import_js.h`), used by `ks_game_load` (server and client hosts), `ksc import-js`, `ksc check` / `ksc run`, editor test play, editor-mcp; tests: `ks_front_test` + `ks_vm_test` over `shared/tests/ks/js/`, `ks_server_test` (compiled-JS server script) |

This document is the only input the implementation agents need. Every rule cites its source.

---

## 0. Sources, provenance and confidence

### 0.1 Abbreviations used in citations

| Tag | Path |
|---|---|
| `REF` | `/home/user/jails/kru/src/Krunker_v7.1.4` (read-only) |
| `API` | `REF/src/libs/createKrunkScriptApiContext.js` (3312 lines, the whole `GAME.*` surface) |
| `KSJ` | `REF/src/libs/krunkScript.js` (prefix/sanitize/parse/minify, 416 lines) |
| `KRT` | `REF/src/libs/krunkScriptRuntime.js` (14 lines) |
| `KCF` | `REF/src/libs/krunkScriptConfig.js` (prefix `V`, blacklist, `maxLoop`, `maxStack`) |
| `KUT` | `REF/src/libs/krunkScriptUtils.js` (prefix helpers) |
| `TYP` | `REF/src/libs/krunkScriptTypes.js` (compiler return-type table) |
| `SYN` | `REF/src/data/krunkScriptSyntax.js` (hook templates, editor autocomplete, aliases) |
| `TPL` | `REF/src/data/scriptTemplates.js` (18 editor templates; scripts are base64 inside `data`) |
| `GJS` | `REF/src/data/game.js` |
| `PLJ` | `REF/src/data/player.js` |
| `PLS` | `REF/src/data/players.js` |
| `IDX` | `REF/src/index.js` (client) |
| `CTL` | `REF/src/libs/controls.js` |
| `GOV` | `REF/src/libs/game-overlay.js` |
| `TRG` | `REF/src/data/triggers.js` |
| `MMG` | `REF/src/libs/MapManager.js` |
| `MAP` | `REF/src/data/map.js` |
| `EDT` | `REF/src/editor.js` |
| `UT`  | `REF/src/libs/utils.js` |
| `CFG` | `REF/src/config.js` |
| `VER` | `REF/public/docs/versions.txt` (official changelog, newest first) |
| `SRV` | `REF/server/index.ts` |

### 0.2 What the reference does and does not contain (read this first)

1. **The KrunkScript compiler is not in the reference.** Official Krunker compiled KrunkScript to
   JavaScript on its backend. The editor sends source with `ksC` (validate) and `ksT` (compile for
   test play) over the editor socket (`EDT:2224-2248`, `EDT:1946-1955`, `EDT:2196-2213`). The local
   v7.1.4 server just echoes the source back (`SRV:3774-3779`). `KSJ:387-408` (`parse`) is only
   `new Function(compiledJs).bind(globals)()`. The local stack therefore cannot run KrunkScript, and
   the page mod strips KrunkScript from sandbox maps (`REF/mod/objects.js:60-100`,
   `REF/mod/devConsole.ts:3057-3080`).
2. The **language** in this spec is reconstructed from:
   * **Compiler output** (`[OUT]`): two published maps ship compiled JS instead of source:
     `REF/public/maps/SniperRoyale2.json` and `REF/public/maps/Verb_Arena.json` (`scripts.client`,
     base64). Their prelude and lowering patterns define the runtime semantics exactly (section 1.16).
   * **Accepted source corpus** (`[CORPUS]`): 15 template scripts from `TPL` (3 templates have
     empty `data`) and 7 map scripts (`kanji`, `laboratory`, `lostworld_s6`, `sanctum`,
     `subzero_new` share one script; `citadel`, `Krunk_Plaza` differ). Everything in them compiled
     on the official compiler, so the KrunkNative (KN) front end must accept all of it.
   * **The official changelog** (`[LOG]`, `VER`), which names every language feature as it shipped.
3. The **runtime API**, hooks, rate limits and engine wiring are read directly from v7.1.4 code
   (`[REF]`).
4. Where none of the above decides a question, this spec makes a KN decision (`[KN]`). Section 7
   lists them as an open-questions register.

**Acceptance principle [KN]:** the KN checker must accept every program the official compiler
accepted (the whole corpus) and must give it the same observable behaviour. It may accept more,
as long as the extra programs have a well-defined meaning here.

### 0.3 Extracting the corpus (for the test agents)

```sh
# map scripts (base64 in scripts.client / scripts.server; SniperRoyale2 and Verb_Arena are compiled JS)
python3 -I -c 'import json,sys,base64;d=json.load(open(sys.argv[1]));print(base64.b64decode(d["scripts"]["client"]).decode())' \
  /home/user/jails/kru/src/Krunker_v7.1.4/public/maps/kanji.json
# templates: TPL is a CommonJS array of {name, data}; data is map JSON whose scripts are base64
node -e 'for(const t of require(process.argv[1])){if(!t.data)continue;const s=JSON.parse(t.data).scripts||{};for(const k in s)console.log("== "+t.name+" "+k+"\n"+Buffer.from(s[k],"base64").toString())}' \
  /home/user/jails/kru/src/Krunker_v7.1.4/src/data/scriptTemplates.js
```

The "Triggers & Scripts" template (`TPL`, entry 14) also contains the reference map for the
trigger -> KrunkScript integration: two trigger objects with `"tas":[{"ta":58,"tct":"1"}]`.

---

## 1. Language

### 1.1 Source text and transport

* A map's scripts live in map JSON as `"scripts": {"client": <b64>, "server": <b64>}`; both keys are
  optional. The editor writes `UTILS.b64e(source)` (UTF-8 -> base64) and omits a script that equals
  its template (`EDT:3519-3526`). The editor imports either base64 or raw text (`b64Test` then `b64d`,
  `EDT:4084-4098`). [REF]
* `game.prepareGame` decodes base64 (`b64d`; the reference decodes as `ascii`, `UT:1310`) when the
  map arrives encoded (`GJS:615-629`). KN: accept a base64 string (strict alphabet, `UT:1311`
  `b64Test`) **or** raw source; decode base64 as UTF-8 (deviation: v7 masks bytes to 7 bits;
  UTF-8 is a superset for ASCII scripts). [KN]
* Before parsing a map, `prepareGame` replaces every `(`/`)` in the whole map JSON with fullwidth
  `（`/`）` (U+FF08/U+FF09) as an injection guard (`GJS:609`); `addScripts` turns them back to ASCII
  in script text (`GJS:163-168`). KN: in script text, map U+FF08 to `(` and U+FF09 to `)` before
  lexing. [REF]
* A script whose text starts with `"use strict";` and contains `const SM = (m) =>` is **compiled
  JS** (2 corpus maps). KN cannot run JS: the WP7 importer (`ks_import_js.h`) recognises the 1.16
  lowering and translates it back to KrunkScript source, which then compiles as usual (the host
  logs `KrunkScript: running the compiled JavaScript script through the importer`). A script
  outside that shape logs `KrunkScript: compiled JavaScript scripts are not supported (<side>
  script, <line>:<col> <reason>)` and runs nothing for that side. [KN]
* Line endings: LF, CRLF or CR. Line/column numbers in diagnostics are 1-based; a tab counts as one
  column. [KN]

### 1.2 Lexical grammar

```
Whitespace     = U+0020 | U+0009 | U+000B | U+000C | U+000D | U+000A | U+00A0 | U+FEFF ;
Comment        = "#" { any char except LF/CR } ;            (* line comment; `#` anywhere outside strings *)
Ident          = ( Letter | "_" ) { Letter | Digit | "_" } ; (* ASCII letters only [KN] *)
Number         = Dec | Hex ;
Dec            = Digits [ "." Digits ] [ ( "e" | "E" ) [ "+" | "-" ] Digits ]
               | "." Digits [ Exp ] ;                         (* leading-dot form: [KN] accepted *)
Hex            = "0" ( "x" | "X" ) HexDigit { HexDigit } ;    (* [LOG] VER:2466 *)
String         = "'" { SChar | Escape | '"' } "'"
               | '"' { SChar | Escape | "'" } '"' ;          (* no raw newline inside *)
Escape         = "\" ( "'" | '"' | "\" | "n" | "r" | "t" | "b" | "f" | "v" | "0"
                     | "x" HexDigit HexDigit | "u" HexDigit{4} | LineTerminator ) ;
```

* Comments: only `#` line comments exist (`[CORPUS]` everywhere; `KSJ:412` `minify` strips
  `^\s*#[^\r\n]+[\r\n]`). Trailing comments after code are allowed (`num x = 70; # comment`).
* Strings: both quote styles appear in the corpus. The compiler copied literals verbatim into JS
  (`'customSky'` stays single-quoted in `SniperRoyale2`), so escape processing is JavaScript's. [OUT]
* Numbers: decimal, fraction, exponent (`1e+3`, `1e-3`, `VER:2467`), hex (`0xAB`, `VER:2466`).
  Numeric values are IEEE-754 doubles. There is no separate integer type.
* Keywords (reserved, cannot be identifiers):
  `num str bool obj action public return if else for while break continue true false
  addTo remove lengthOf notEmpty hasProp toNum toStr static dynamic`.
* Predeclared names (reserved as identifiers): `GAME`, `UTILS` (= `GAME.UTILS`), `Math`
  (= `GAME.UTILS.math`) (`SYN:640-643`; compiled prelude `let V_UTILS = this.GAME.V_UTILS;
  let V_Math = this.GAME.V_UTILS.V_math;` [OUT]).
* JavaScript reserved words are **ordinary identifiers** because every user name is emitted with a
  `V_` prefix (`KCF` `prefix: "V"`, `KUT` `prefix.add`). The corpus uses `new` and `function` as
  variable names (`Snake Demo`, `Game of Life`). [CORPUS]
* Restricted names [REF `KCF` blacklist; enforcement point is KN]: `prototype process require
  mainModule Module __compile constructor __defineGetter__ __defineSetter__ hasOwnProperty
  __lookupGetter__ __lookupSetter__ isPrototypeOf propertyIsEnumerable valueOf __proto__ arguments
  caller apply bind call`. KN rejects them as variable, parameter, action **and** property/object-key
  names with `Restricted name "<name>".`
* Punctuators: `{ } ( ) [ ] ; , . : ? = += -= *= /= %= **= <<= >>= >>>= &= |= ^= ++ -- + - * / % **
  == != < <= > >= << >> >>> & | ^ ~ ! && ||`.
  * `==`/`!=` are emitted as `===`/`!==` (`V_id === 'customSky'`, Verb_Arena). [OUT]
  * `+=`/`-=` `[LOG VER:2463]`, `*=` `[CORPUS Sprinting]`; the other compound forms are [KN].
  * Bitwise `~ << >> >>> & | ^` and `**` `[LOG VER:2464-2465]`.
  * There is no `===` token in source. [KN: lex error `Unexpected "=".`]

### 1.3 Grammar (EBNF)

```
program        = { topItem } EOF ;
topItem        = globalDecl | actionDecl | ";" ;
globalDecl     = varDecl ";" ;                          (* initialiser runs once at load, 2.2 *)

actionDecl     = [ "public" ] [ type ] "action" Ident "(" [ paramList ] ")" block ;
paramList      = param { "," param } [ "," ] ;
param          = [ "static" | "dynamic" ] paramType Ident ;
paramType      = type | actionType ;
actionType     = [ type ] "action" [ "(" [ type { "," type } ] ")" ] ;   (* VER:1286, form [KN] *)

type           = baseType { "[" "]" } ;
baseType       = "num" | "str" | "bool" | "obj" ;

block          = "{" { statement } "}" ;
statement      = block
               | varDecl ";"
               | ifStmt | whileStmt | forStmt | forEachStmt
               | "return" [ expr ] ";"
               | "break" ";" | "continue" ";"
               | "addTo" unary expr ";"
               | "remove" postfix ";"
               | simpleStmt ";"
               | ";" ;                                  (* `};` after blocks is legal, VER:2668 *)
varDecl        = [ "static" | "dynamic" ] type Ident [ "=" expr ] ;   (* "= expr" optional [KN] *)
ifStmt         = "if" "(" expr ")" body [ "else" ( ifStmt | body ) ] ;
whileStmt      = "while" "(" expr ")" body ;
forStmt        = "for" "(" [ varDecl | simpleStmt ] ";" [ expr ] ";" [ simpleStmt ] ")" body ;
forEachStmt    = "for" "(" type Ident ":" expr ")" body ;            (* VER:1253, syntax [KN] *)
body           = block | statement ;                    (* corpus always uses blocks; others [KN] *)
simpleStmt     = assignTarget assignOp expr
               | assignTarget ( "++" | "--" )
               | ( "++" | "--" ) assignTarget
               | postfix ;                              (* must be a call: Expression statement *)
assignTarget   = [ cast ] postfix ;                     (* postfix must end in Ident, .Ident or [expr] *)
assignOp       = "=" | "+=" | "-=" | "*=" | "/=" | "%=" | "**=" | "<<=" | ">>=" | ">>>=" | "&=" | "|=" | "^=" ;

expr           = ternary ;
ternary        = logicOr [ "?" expr ":" expr ] ;        (* right-assoc; CORPUS Basic Networking *)
logicOr        = logicAnd { "||" logicAnd } ;
logicAnd       = bitOr { "&&" bitOr } ;
bitOr          = bitXor { "|" bitXor } ;
bitXor         = bitAnd { "^" bitAnd } ;
bitAnd         = equality { "&" equality } ;
equality       = relational { ( "==" | "!=" ) relational } ;
relational     = shift { ( "<" | "<=" | ">" | ">=" ) shift } ;
shift          = additive { ( "<<" | ">>" | ">>>" ) additive } ;
additive       = multiplicative { ( "+" | "-" ) multiplicative } ;
multiplicative = power { ( "*" | "/" | "%" ) power } ;
power          = unary [ "**" power ] ;                 (* right-assoc; unary LHS is an error, 1.6 *)
unary          = ( "!" | "-" | "+" | "~" | "lengthOf" | "notEmpty" | "toNum" | "toStr" ) unary
               | "hasProp" postfix                      (* postfix must end in .Ident *)
               | cast unary
               | postfix ;
cast           = "(" type ")" ;                         (* `(` + type keyword => cast, never a group *)
postfix        = primary { "(" [ argList ] ")" | "." Ident | "[" expr "]" } ;
argList        = expr { "," expr } [ "," ] ;            (* trailing comma: CORPUS Game of Life *)
primary        = Number | String | "true" | "false" | Ident
               | "(" expr ")"
               | arrayLit | objectLit ;
arrayLit       = baseType { "[" "]" } "[" [ expr { "," expr } [ "," ] ] "]" ;
objectLit      = "{" [ objEntry { "," objEntry } [ "," ] ] "}" ;   (* trailing comma: CORPUS kanji *)
objEntry       = ( Ident | String ) ":" expr ;          (* string keys [KN] *)
```

Notes on the grammar:

* **Top level** holds only global variable declarations and action declarations [CORPUS]. A bare
  statement at top level is a compile error `Statements are only allowed inside actions.` [KN].
  Global initialisers may call `GAME.*` (`obj bounds = GAME.OVERLAY.getSize();` in Snake Demo,
  `num oldTime = GAME.TIME.now();` in Game of Life). [CORPUS]
* **Array literal disambiguation.** In expression position a base-type keyword starts an array
  literal. Read the base type, then every `[` `]` pair; the **last** bracket group is the element
  list, the pairs before it add dimensions to the element type:
  * `num[1, 2]`: element type `num`, two elements, value type `num[]`.
  * `obj[]`: element type `obj`, empty, value type `obj[]`.
  * `num[][]`: element type `num[]`, empty, value type `num[][]`.
  * `str[][str['q','w'], str['a']]`: element type `str[]`, value type `str[][]`.
  * Whitespace between parts is allowed: `num[] [num [(width - 40) / 2, (height - 40) / 2]]` and
    `num [old[0] + 1, old[1]]` (Snake Demo). [CORPUS]
  * Parsing procedure: after the base type, consume bracket groups while the next token is `[`. An
    empty group `[]` followed by another `[` is a dimension; an empty group not followed by `[` is the
    (empty) element list; a non-empty group is the element list and ends the literal, so a later `[`
    is ordinary indexing. In statement position a type keyword always starts a declaration.
* **Statement-level `{`** opens a block; an object literal only appears in expression position.
* **`addTo`**: the first operand is a `unary` (so a cast is allowed: `addTo (num[][]) player.pos new;`)
  and must be an array. The second operand is a full expression (`addTo buffer {x: i, y: j, val: 1};`). [CORPUS]
* **`remove`** takes a postfix ending in `[index]` (array element) or `.key` (object property). [CORPUS Snake Demo, VER:1917]
* **Cast on an assignment target** (`(num) ball.x += ...;`, `(num) player.tail++;`) declares the static
  type of a dynamic l-value so that compound assignment and `++`/`--` type-check. It does **not** convert
  the stored value first. [CORPUS; lowering 1.16 shows plain `(V_i)++`]
* Parenthesised `if`/`while`/`for` conditions are mandatory. `do`, `switch`, `try`, `const`,
  `null`, nested actions, lambdas and closures **do not exist**.

### 1.4 Types

| Type | Values | Default value [KN] | JS representation (v7) |
|---|---|---|---|
| `num` | IEEE double, incl. `NaN`, `Infinity`, `-0` | `0` | number |
| `str` | immutable string (UTF-16 semantics, 1.12) | `""` | string |
| `bool` | `true` / `false` | `false` | boolean |
| `obj` | reference to a property bag: script object, API object (host wrapper), or empty (`null`/`undefined` from the API) | `{}` | object / Proxy / null |
| `T[]` (any depth) | reference to a growable array whose elements are statically `T` | `T[]` (empty) | Array |
| `action` / `T action(...)` | reference to a top-level script action, or to a host function | none (must be initialised) | bound function |
| `any` (internal only) | result of `obj.prop`, `obj.method()`, `arr` of `any`, untyped API returns | — | any JS value |
| `void` (internal only) | the result of an action without a return type | — | `undefined` |

* There is no source keyword `any`/`void`. They are checker-internal types. [KN]
* `static`/`dynamic` (`static obj inputs`, `dynamic obj data` in templates) are accepted modifiers
  on a declaration or parameter and have **no effect** [CORPUS accepted; meaning unknown => KN].
* Typed arrays: `num[]`, `str[][]`, `obj[]`... An array literal must give its element type
  (`num[1,2]`, not `[1,2]`). [CORPUS]
* `null`/`undefined` cannot be written but do occur: API functions return them
  (`PLAYERS.findByID` returns `null`, `getSelf` on the server returns `null`, `UI.addDIV` failure
  returns `undefined`, a missing property reads `undefined`). KN models both as distinct values
  `null` and `undefined` because their string forms differ (`"null"` vs `"undefined"`) and
  `JSON` serialisation differs (1.12, 2.5).

### 1.5 Type-checking rules

Static types are attached to every expression. `any` is the dynamic type.

1. **Declarations.** `T name = e;` requires `e : T`, or `e : any` (an implicit cast `(T)` is
   inserted and a warning `Implicit cast of dynamic value to T.` is emitted [KN]; the corpus always
   writes the cast, e.g. `num width = (num) bounds.width;`, so this never changes corpus behaviour).
   Arrays and objects are distinct types: an `obj` slot does not accept a `T[]` value and vice versa [KN].
   Redeclaring a name in the same scope: `Variable "x" is already defined.`
2. **Literals**: number `num`, string `str`, `true/false` `bool`, `{...}` `obj`, `T[...]` `T[]`.
3. **Identifiers** have their declared type. An action name used without a call has type `action`.
4. **Member access** `e.name`: `e` must be `obj` or `any` (`Cannot read property of <type>.`
   otherwise). Result type: `any`, except for typed `GAME.*` paths (rule 9).
5. **Index** `e[i]`: `e : T[]` gives `T`; `e : str` gives `str` (one UTF-16 unit) [KN]; `e : any`
   gives `any`. `i` must be `num` or `any` (`Array index must be a number.`).
6. **Calls**
   * Script action: arity must match exactly (`Action "f" expects N arguments.`), each argument must
     be assignable (rule 1, with `any` implicitly cast). Result: declared return type, or `void`.
     Using a `void` result as a value: `Action "f" does not return a value.`
   * `action`-typed variable/parameter: any arguments, result `any`.
   * `GAME.*` function: arguments are **untyped** (any count; missing ones are `undefined`, extras
     ignored, exactly like JS). Result: rule 9.
   * Method on `obj`/`any` (`torch.move(...)`, `tPlr.disableDefault("jump")`, `dat.getBone("Palm2L")`):
     untyped arguments, result `any`; runtime check that the property is callable (1.14).
7. **Operators**
   * `+`: `num+num -> num`; `str+str -> str`. `str+num` / `num+str` is a compile error
     `Cannot add str and num; use (str) or toStr.` (the corpus always converts: `'keypad' + (str)i`,
     `toStr grid.scale + "px"`) [CORPUS]. If either side is `any`, result is `any` and the JS `+`
     algorithm applies at runtime (1.12) [KN].
   * `- * / % ** & | ^ << >> >>> ~` and unary `-`/`+`: operands `num` (or `any` -> implicit
     `ToNumber`, result `num`) [KN]. Result `num`.
   * `== !=`: both sides the same static type, or either side `any` [KN]. Result `bool`.
     Comparing `num` with `str` statically: `Cannot compare num and str.`
   * `< <= > >=`: `num` with `num`, `str` with `str` (lexicographic), or `any` on either side [KN].
   * `! && || ?:` conditions accept **any type** (JS truthiness): `if (inputs.jump)`, `!!tPlr`,
     `!!inputs.movDir` [CORPUS]. `&&`/`||` produce `bool` when both operands are `bool`; otherwise
     they return one of the operands (JS) and the result type is `any` [KN].
     `?:` result type: the common type of both branches, else `any`.
   * `++ -- op=` targets must be `num` (or `str` for `+=`) statically; a dynamic target needs a cast
     prefix: `(num) ball.x += 1;` [CORPUS]. Without it: `Cannot apply "+=" to a dynamic value; add a cast.`
8. **Keyword operators**: `lengthOf e` (`e : str | T[] | any`) -> `num`; `notEmpty e` (any) -> `bool`;
   `hasProp o.k` -> `bool`; `toNum e` -> `num`; `toStr e` -> `str`; `(T) e` -> `T`;
   `addTo a v` requires `a : T[]` (or cast to it) and `v` assignable to `T`;
   `remove a[i]` / `remove o.k`.
9. **Typed `GAME.*` results** come from `TYP` (the compiler's table; keys omit the `GAME.` root and
   may use `*`). A path not in the table returns `any` [KN] (`TYP` lists plain `action()` = void, but
   the corpus never uses such a result, and returning `any` keeps `str v = GAME.SETTINGS.get(...)` style
   code working).

   | `TYP` key | Type | | `TYP` key | Type |
   |---|---|---|---|---|
   | `UTILS.math.*` | `num` | | `UI.addDIV` | `str` |
   | `UTILS.randInt` `randFloat` `getDir2D` `getDist2D` `getDist3D` `anglDst` | `num` | | `UI.addImage` `moveDIV` `getDIVText` `getProp` | `str` |
   | `UTILS.getDir3D` | `obj` | | `UI.getSize` | `obj` |
   | `UTILS.hexFromHue` `invertHex` `RGBtoHEX` `toLower` `toUpper` `truncateTxt` `replaceText` | `str` | | `SETTINGS.get` `SETTINGS.list` | `str` (sic: `list` really returns an array) |
   | `UTILS.HEXtoRGB` | `obj` | | `OVERLAY.getSize` | `obj` |
   | `UTILS.textContains` | `bool` | | `OVERLAY.measureText` | `num` |
   | `TIME.now` `TIME.fixedDelta` | `num` | | `COOKIES.load` | `str` |
   | `TIME.getReadable` | `str` | | `COOKIES.has` | `bool` |
   | `INPUTS.keyDown` | `bool` | | `NETWORK.send` `NETWORK.broadcast` | `bool` |
   | `INPUTS.mousePos` `mouseMovement` `mousePosOverlay` | `obj` | | `NFT.hasWallet` | `bool` |
   | `PLAYERS.list` | `obj[]` | | `NFT.ownedAssets` | `obj[]` |
   | `PLAYERS.getSelf` `findByID` | `obj` | | `SCENE.add*` `SCENE.posToScreen` | `obj` |
   | `AI.list` | `obj[]` | | `AI.spawn` | `obj` |
   | `RAYCAST.from` | `obj` | | `OBJECTS.getPathNodes` `OBJECTS.list` | `obj[]` |
   | `SOUND.play*` | `obj` | | `PLATFORM.isClient` `isBrowser` `isMobile` | `bool` |
   | `CAMERA.getObj` | `obj` | | `CONFIG.getHost` | `obj` |

   `GAME.SETTINGS.list` is typed `str` but returns an array. KN keeps the reference type `str` for
   checking and returns the array at runtime, so a script that assigns it to a `str` gets an array in
   a `str` slot, exactly as in v7 [REF quirk, kept].
10. **Unknown `GAME` member** (`GAME.FOO.bar`) is a compile error `Unknown member "GAME.FOO.bar".` [KN]
    (the official compiler validated against the API, `SYN` autocomplete; exact behaviour unknown).
11. **Return**: in a typed action every `return` needs an expression assignable to the return type;
    `return;` only in untyped actions. Falling off the end of a typed action returns the type's
    default [KN] (JS returned `undefined`; the corpus never relies on it).
12. **Public hooks**: a `public action` whose name is a hook (section 4) must declare at most the
    hook's parameters (fewer is fine: `onCustomTrigger(str playerID, str customParam)` in templates
    omits `value`). Parameter types are not checked against the hook table [CORPUS]. Public actions
    with other names are allowed and simply never called by the engine [KN].

### 1.6 Operators: precedence and semantics

Highest first (same table as JavaScript, which the compiler emitted into):

| Level | Operators | Assoc | Semantics |
|---|---|---|---|
| 1 | `f(..)` `a.b` `a[i]` `x++` `x--` | left | call, member, bounds-checked index (1.14), post-inc/dec |
| 2 | `!` `-` `+` `~` `++x` `--x` `(T)` `lengthOf` `notEmpty` `hasProp` `toNum` `toStr` | right | unary |
| 3 | `**` | right | `Math.pow`. An unparenthesised `!`/`-`/`+`/`~` operand on the left (`-a ** b`) is a compile error `Wrap the unary operand of "**" in parentheses.` (a JS SyntaxError in v7). Casts and keyword operators on the left are fine (`(num) x ** 2`), because v7 lowered them to function calls |
| 4 | `*` `/` `%` | left | JS: `/` by 0 gives `±Infinity`/`NaN`; `%` = C `fmod` (sign of dividend) |
| 5 | `+` `-` | left | `+` numeric or string concatenation (1.12) |
| 6 | `<<` `>>` `>>>` | left | ToInt32 / ToUint32, shift count `& 31` |
| 7 | `<` `<=` `>` `>=` | left | numeric, or lexicographic by UTF-16 code units when both are strings |
| 8 | `==` `!=` | left | **strict** equality (`===`): same type and value; NaN != NaN; objects/arrays by identity |
| 9 | `&` | left | ToInt32 |
| 10 | `^` | left | ToInt32 |
| 11 | `\|` | left | ToInt32 |
| 12 | `&&` | left | JS short-circuit, returns an operand |
| 13 | `\|\|` | left | JS short-circuit, returns an operand |
| 14 | `?:` | right | |
| 15 | `=` `op=` (statements only) | right | |

Unary keyword operators bind like `!`: `lengthOf s + 1` is `(lengthOf s) + 1`;
`toStr (t / 1000) + "s"` is `(toStr (t / 1000)) + "s"`; `(num) coords.y+40` is `((num) coords.y) + 40`;
`lengthOf (num[][]) player.pos > (num) player.tail` is `(lengthOf ((num[][]) player.pos)) > ((num) player.tail)`. [CORPUS]

Assignment, `++`/`--` are statements, not expressions [KN; the corpus never nests them].

### 1.7 Control flow

* `if (c) {..} else if (c) {..} else {..}` — `c` uses JS truthiness (falsy: `false`, `0`, `-0`, `NaN`,
  `""`, `null`, `undefined`; everything else, including `"0"`, `{}`, `[]`, true).
* `while (c) {..}`.
* `for (init; cond; step) {..}` — `init` may declare (`for (num i = 0; ...)`); the variable is
  scoped to the loop (JS `let`, one binding per iteration is irrelevant without closures). [OUT]
* `for (T v : e) {..}` — iterates **object properties** (`VER:1253`). Lowering uses
  `OE(x) = Object.entries(x||{}).map(p => ({V_key: RP(p[0]), V_value: p[1]}))` [OUT prelude], so
  each element is a fresh `obj` with `key` (`str`, the un-prefixed property name) and `value`
  (`any`). KN grammar: `for (obj entry : someObj) { GAME.log(entry.key, entry.value); }`; `T` must
  be `obj` [KN]. Iteration order = property insertion order (all KS keys carry the `V_` prefix so
  JS's integer-key ordering never applies). The entry list is snapshotted before the first iteration.
  Iterating a non-object (array, string, number, `null`) yields nothing for `null`/`undefined` and
  for numbers/bools; for arrays/strings `Object.entries` yields index keys: KN reproduces this
  (`key` = `"0"`, `"1"`...). [REF semantics of `Object.entries`]
* `break;` / `continue;` inside loops (`VER:2703`). Outside a loop: compile error
  `"break" outside of a loop.`
* `return [e];` (any action). In a public hook the return value is ignored.
* Every loop iteration counts against the per-entry loop budget (1.14).

### 1.8 Actions, hooks and callbacks

* `[public] [T] action name(params) { ... }`. `T` is the return type (`num action neighbors(num x, num y)`,
  `obj action genPebble()`). [CORPUS]
* Actions are hoisted: any action may call any other regardless of order (JS function declarations). [OUT]
* `public` actions are exported; the engine calls exported actions whose names are hooks (section 4).
  The compiled module returns `{<each public action>: V_x.bind(this), <every other client hook>:
  function(){}}` (`SniperRoyale2`, `Verb_Arena` epilogues) [OUT].
* Non-public actions are callable only from script code.
* Recursion is allowed; depth is limited (1.14).
* **Action values** (`VER:1286`): an action name used without `()` is a value of type `action`; it can
  be passed to a parameter of `action` type and to `GAME.*` callbacks (`STORAGE.load`, `STORAGE.set`,
  `STORAGE.update`, `NFT.ownedAssets`, `SCENE.addAsset`'s `onLoadCallBack`). Calling an action value
  with the wrong number of arguments pads with `undefined` / drops extras (JS). [KN for the call rules]
* There are **no closures**: actions cannot be declared inside actions, and action values only
  refer to top-level actions or host functions. An action value captures nothing.

### 1.9 Variables and scopes

* Globals: top-level `varDecl`s; lowered to module-level `let` (`let V_usingMenu = false;`) [OUT].
  They live for the lifetime of the loaded script (2.2).
* Locals: block-scoped (`let`); parameters are locals of the action. Shadowing an outer name in an
  inner block is allowed [KN, JS `let` semantics]; redeclaring in the same block is an error.
* Use before declaration (in the same or an enclosing scope): compile error `Variable "x" is not defined.` [KN]
* Assignment copies **references** for `obj` and arrays (`obj oldWindow = window;` aliases) and
  values for `num`/`str`/`bool`.

### 1.10 Objects

* An `obj` is a property bag with string keys and insertion order. Script code sees un-prefixed
  names; v7 stores them as `V_<name>` (section 1.16). Missing properties read as `undefined`.
* Creation: object literal `{a: 1, b: "x", c: {d: true}}` (nested literals allowed, trailing comma ok).
* Read/write: `o.a`, `o.a = 5`, chained `lamp.light.position.x = ...` [CORPUS Moving Objects].
* Add: assigning a missing key creates it (`VER:1917` "KS objects are now more dynamic").
* Delete: `remove o.a;` (lowered to JS `delete`; `SM` rewrites `delete` to `remove` in error text) [OUT].
* Test: `hasProp o.a` (lowered `HP(o, "V_a")` = `o.hasOwnProperty("V_a")`; `VER:1289`) [OUT].
* Non-empty: `notEmpty o` (`NE(x) = !!Object.keys(x||{}).length`) — true for any object with at least
  one own key, any non-empty string or array, false for `{}`, `""`, `[]`, `null`, `undefined`, numbers. [OUT]
* Iterate: `for (obj e : o)` (1.7).
* Dynamic key access (`o[k]`) is **not** supported: the index operator is array indexing (1.14).
* **API (host) objects** returned by `GAME.*` (players, AIs, scene objects, sounds...) are `obj`
  values with getter/setter properties and methods (section 3.2). Script code may add its own
  properties to them; for players/AIs/sounds/obj3Ds the custom properties persist on the engine
  object across later fetches (`player._protected` cache, `API:54`, `KSJ:276-293`, `VER:1678`).
  Built-in property **objects** of a host object (`position`, `rotation`, `velocity`, `scale`) cannot be
  replaced (`player.position = {}` is silently ignored: non-writable, `KSJ:262-266`) but their fields can be
  set.
* Methods: calling `o.f(...)` requires `o.f` to be callable, else the runtime error
  `Property "f" is not an an "action".` (sic, double "an") [OUT `FC`].

### 1.11 Arrays

* Typed, growable, reference semantics. Elements of `T[]` are statically `T` but at runtime may hold
  anything another path stored there (e.g. a network `obj` field) [REF: plain JS arrays].
* Literal: `num[1, 2]`, `obj[]` (empty), nested `str[][str['a'], str['b']]`.
* Read/write: `a[i]`, `a[i] = v`, `a[i][j]`. Every index is bounds-checked (1.14); arrays never
  grow by index assignment. [OUT `BC`; applying `BC` to writes is KN, see 7]
* Append: `addTo a v;` (JS `push`). [CORPUS]
* Delete element: `remove a[i];` (lowered `DE(x,i)` = `x.splice(i,1)` when `x` is an array, else
  no-op) — later elements shift down. [OUT]
* Length: `lengthOf a` (`L(x) = x.length||0`; `0` for anything without a length). [OUT]
* Casting an array (`(num[]) x`, and the scalar casts applied to an array value) converts every
  element **in place**, recursively (`A(x,f)` mutates and returns the same array) [OUT]; see 1.13.

### 1.12 Strings, numbers and conversions (JavaScript semantics)

Strings are immutable sequences of **UTF-16 code units** in v7. KN stores UTF-8 but every
length/index/substring operation visible to scripts (`lengthOf`, `s[i]`, `UTILS.truncateTxt`,
`OVERLAY.measureText` input, comparisons) must use UTF-16 code-unit positions [KN requirement for
parity]. ASCII fast path: when a string is pure ASCII the byte index equals the unit index.

Conversions used by the runtime (exact algorithms, KN must implement them in C):

| Name | Used by | Definition |
|---|---|---|
| `ToString(v)` | `S()` = `(str)`, `toStr`, `+` with a string, `GAME.log`, API `x + ""` | `num`: ECMAScript Number::toString (shortest round-trip digits; `1e21` and above and below `1e-6` use exponent form `1.5e+21`, `1e-7`; `-0` -> `"0"`; `NaN`, `Infinity`, `-Infinity`). `bool`: `"true"`/`"false"`. `null`: `"null"`. `undefined`: `"undefined"`. array: elements joined with `","` (null/undefined elements -> `""`, nested arrays recurse). object: `"[object Object]"`. action: the JS source text of the function in v7 — KN returns `"[action]"` [KN]. |
| `parseFloat` | `F()` = `(num)`, `toNum` [KN], API args | skip leading JS whitespace; optional sign; `Infinity`; else the longest decimal prefix `digits[.digits][e[+-]digits]`; otherwise `NaN`. Input is `ToString(v)` first, so `(num) true` = `parseFloat("true")` = NaN -> **0** after `\|\| 0`. Hex is **not** recognised (`"0x10"` -> 0). |
| `parseInt` | `N()` (array index), API `parseInt` | `ToString(v)`, skip whitespace, sign, `0x`/`0X` -> radix 16, else radix 10, longest digit prefix, else NaN. `N(x) = parseInt(x) \|\| 0`. |
| `ToNumber` | `-`, `*`, unary `-`, relational on `any`, bitwise | `undefined` -> NaN, `null` -> 0, bool -> 0/1, string -> StringToNumber (`""`/whitespace -> 0, `"0x1F"` -> 31, `"12abc"` -> NaN, `"Infinity"`), object -> NaN, array -> `ToNumber(ToString(a))`. |
| `ToBoolean` | `B()` = `(bool)`, conditions, `!` | falsy: `false 0 -0 NaN "" null undefined`. |
| `ToInt32` | bitwise | JS modulo-2^32 truncation. |
| `+` (any) | `a + b` with an `any` side | if either `ToPrimitive` result is a string -> concatenate `ToString`s, else numeric add of `ToNumber`s. `ToPrimitive(array)` = `ToString(array)`, `ToPrimitive(obj)` = `"[object Object]"`. |

Number formatting is the most visible JS dependency (`toStr 0.1 + 0.2` must print `0.30000000000000004`,
`toStr 1/3` `0.3333333333333333`, `toStr 1e21` `1e+21`, `toStr 123456789012345680000` `123456789012345680000`).
Implement shortest round-trip digit generation (Ryu, Grisu with fallback, or the simple loop: for
`p` in 1..17 print `%.{p-1}e`, `strtod` it back, stop at the first exact match) and then apply the
ECMAScript layout rules (Number::toString steps 6-10). This lives in `shared/src/ks/ks_number.c`.

### 1.13 Keyword operators and casts (exact runtime semantics)

| Source | v7 lowering [OUT] | Semantics |
|---|---|---|
| `(num) e` | `A(e, F)` | `F(x) = parseFloat(x) \|\| 0` (so NaN -> 0, but Infinity stays). Arrays: in place, element-wise, recursive. |
| `(str) e` | `S(e)` | `S(x) = x + ""` (`ToString`). Arrays: **KN applies `A(e,S)` (element-wise, in place)** for consistency with `(num)`; v7 output showed `S()` without `A` (Verb_Arena) for a non-array operand only. See 7. |
| `(bool) e` | `B(e)` | `!!x`. Arrays as above [KN]. |
| `(obj) e` | `O(e)` | `typeof x === "object" && !Array.isArray(x) ? x : {}` — **`null` -> `null`** (JS `typeof null === "object"`). Arrays: element-wise [KN]. |
| `(T[]) e`, `(T[][]) e` | `A(e, f_T)` | element-wise in place with `T`'s scalar function; a non-array operand is converted as a scalar (so `(num[]) 5` yields `5`, not an array) [OUT `A`]. |
| `toNum e` | `A(e, F)` [KN] | same as `(num)` (`VER:2636` "Fixed toNum"). |
| `toStr e` | `S(e)` | same as `(str)`. |
| `lengthOf e` | `L(e)` | `e.length \|\| 0` (strings: UTF-16 units; arrays: length; everything else 0). |
| `notEmpty e` | `NE(e)` | `!!Object.keys(e \|\| {}).length`. |
| `hasProp o.k` | `HP(o, "V_k")` | own-property test; `o` null/undefined -> runtime error (JS TypeError). |
| `addTo a v` | `a.push(v)` | `a` not an array -> runtime error `a.push is not a function` (via `EH`). |
| `remove a[i]` | `DE(a, i)` | `splice(i,1)`; out of range or non-array: no-op, no error. |
| `remove o.k` | `delete o.V_k` | deleting a missing key is a no-op; deleting a host property: no-op (non-configurable) [REF `KSJ:248`]. |
| `a[i]` | `a[BC(a,i,line,col)]` | `BC` = `a.hasOwnProperty(N(i)) ? N(i) : RE(line,col,"Index out of range.")`. So `a[1.7]` reads index 1, `a[-1]` errors, `a["2"]` reads 2. |
| `o.f(args)` | `FC(o.V_f, "f", line, col)(args)` | callable check (1.10). |

### 1.14 Runtime limits and runtime errors

**Error delivery.** Each public action body is wrapped (`try { LC = 0; ... } catch (e) { EH(e.message) }`)
[OUT]; `EH` rethrows `Error("RUNTIME ERROR: " + SM(msg))`, and the wrapper built by `KSJ:389-406`
catches it and calls `console.error([message])`. Effects:

* A runtime error **aborts only the current hook invocation**. Script state (globals) keeps the
  values written before the error. The script stays loaded and the next hook call runs normally.
* The message goes to the script console (2.8) as an error.
* `SM(m)` clean-up: replace `this.GAME` with `GAME`, strip `V_` prefixes (`V_(\w+)` -> `$1`),
  replace the word `delete` with `remove`, drop `of #<Object>`.
* An error thrown while running **global initialisers** (script load) is not caught by a hook
  wrapper: in v7 it propagates out of `runtime.parse` and `prepareGame` returns `"Map error occured."`
  (sic) (`GJS:673-676`), i.e. the map fails to load. KN: the script (that side) is disabled and the
  error is logged; the map still loads [KN deviation: a broken script must not brick a map in KN,
  where the same compiler runs on both sides].

**Errors raised by generated code** (exact text, `<l>:<c>` = source line:column of the construct):

| Message | Trigger |
|---|---|
| `RUNTIME ERROR: <l>:<c> Loop timed out.` | loop budget exceeded (below) |
| `RUNTIME ERROR: <l>:<c> Index out of range.` | `BC` failure, read **or** write |
| `RUNTIME ERROR: <l>:<c> Property "<name>" is not an an "action".` | `FC` failure |
| `RUNTIME ERROR: Cannot read properties of undefined (reading '<name>')` | member read on `undefined` (JS TypeError text, V_ stripped) |
| `RUNTIME ERROR: Cannot read properties of null (reading '<name>')` | member read on `null` |
| `RUNTIME ERROR: Cannot set properties of undefined (setting '<name>')` / `... of null ...` | member write |
| `RUNTIME ERROR: <expr> is not a function` | calling a non-callable action value / `addTo` on a non-array (`<expr>` = source text of the callee, KN) |
| `RUNTIME ERROR: Maximum call stack size exceeded` | call depth > 2000 (`KCF` `maxStack: 2000`) [KN applies it to runtime depth] |
| `RUNTIME ERROR: Internal Error "<path>"` | a `GAME.*` host function failed (`KSJ:296-304` wraps every API function; `<path>` is the dotted path inside `GAME`, e.g. `RAYCAST.from`) |

**Loop budget.** One counter `LC` per script instance. It is reset to 0 at the start of every
host -> script entry (hook call, API callback). Each loop iteration of any loop executes
`if (LC++ > 1e6) RE(...)` before its body [OUT; `KCF` `maxLoop: 1000000`]. So at most 1,000,001
iterations (summed over all loops and all actions called) run per entry; the 1,000,002nd raises
`Loop timed out.` with the position of the loop that hit the limit. (In v7 non-public actions may also
reset `LC`; KN resets only on host entry so the budget is a hard bound — section 7.)

**KN safety limits [KN]** (not in v7; chosen so that no corpus script hits them):

| Limit | Value | Error |
|---|---|---|
| VM instructions per host entry | 50,000,000 | `Execution timed out.` |
| Call depth | 2000 | `Maximum call stack size exceeded` |
| String length | 1,048,576 UTF-16 units | `Invalid string length` |
| Array length | 1,000,000 | `Invalid array length` |
| Object properties | 65,536 per object | `Too many properties.` |
| Heap per script instance | 64 MiB | `Out of memory.` (script disabled) |

### 1.15 Compile errors (KN catalogue)

v7's compiler messages are not available. KN uses `"<line>:<col> <message>"` (same shape as `RE`)
and stops at the first error per script [KN]. Catalogue:

| Message | |
|---|---|
| `Unexpected "<tok>".` / `Unexpected end of script.` | parse |
| `Unterminated string.` / `Invalid number "<text>".` / `Invalid escape sequence.` | lex |
| `Expected "<tok>" but found "<tok>".` | parse |
| `Statements are only allowed inside actions.` | top level |
| `Restricted name "<name>".` | blacklist (1.2) |
| `Variable "<x>" is not defined.` / `Variable "<x>" is already defined.` | scope |
| `Action "<f>" is not defined.` / `Action "<f>" is already defined.` / `Action "<f>" expects <n> arguments.` | calls |
| `Action "<f>" does not return a value.` | void used as value |
| `Cannot assign <T1> to <T2>.` | types |
| `Cannot add <T1> and <T2>; use (str) or toStr.` | `+` |
| `Cannot compare <T1> and <T2>.` | `== != < ...` |
| `Operator "<op>" needs num operands.` | arithmetic/bitwise |
| `Cannot apply "<op>" to a dynamic value; add a cast.` | compound assignment / `++` on `any` |
| `Cannot read property of <T>.` | member on non-obj |
| `Array index must be a number.` / `Cannot index <T>.` | index |
| `"addTo" needs an array.` / `"remove" needs an array element or object property.` / `"hasProp" needs an object property.` | keyword ops |
| `"break" outside of a loop.` / `"continue" outside of a loop.` | |
| `Missing return value.` / `Unexpected return value.` | return |
| `Unknown member "GAME.<path>".` | API path |
| `Wrap the unary operand of "**" in parentheses.` | `**` |
| `Hook "<name>" takes at most <n> parameters.` | hooks |
| `Script too large.` | source > 1 MiB [KN] |

Warnings (logged, compilation continues): `Implicit cast of dynamic value to <T>.`

### 1.16 Reference lowering (what the v7 compiler emitted) — normative semantics

The compiled prelude, verbatim from `SniperRoyale2.json`/`Verb_Arena.json` [OUT]:

```js
"use strict";
const process = {}
const require = {}
const SM = (m) => m.replace(/\bthis\.GAME\b/g, "GAME").replace(/V_(\w+)/g, "$1").replace(/\bdelete\b/g, "remove").replace(/of #<Object>/g, "");
const EH = (m) => {throw new Error("RUNTIME ERROR: "+SM(m).replace(/^RUNTIME ERROR: /g, ""))};
const RE = (l,c,m) => {throw new Error(`${l}:${c} ${m}`)};
const RP = (s) => s.replace(/V_(\w+)/g, "$1");
const L = (x) => (x.length||0)
const NE = (x) => !!Object.keys(x||{}).length
const B = (x) => !!x
const N = (x) => (parseInt(x)||0);
const F = (x) => (parseFloat(x)||0);
const S = (x) => (x+"");
const O = (x) => (typeof x==="object"&&!Array.isArray(x)?x:{});
const A = (x,f) => (Array.isArray(x)?(x.forEach((v,i)=>x[i]=A(v,f)),x):f(x));
const HP = (x,p) => x.hasOwnProperty(p);
const BC = (a,i,l,c) => (a.hasOwnProperty(N(i))?N(i):RE(l,c,"Index out of range."));
const FC = (f,p,l,c) => (typeof f==="function"?f:RE(l,c,`Property "${p}" is not an an "action".`));
const DE = (x,i) => (Array.isArray(x)?x.splice(i,1):0);
const OE = (x) => Object.entries(x||{}).map(p=>{return{V_key:RP(p[0]),V_value:p[1]}});
let V_UTILS = this.GAME.V_UTILS;
let V_Math = this.GAME.V_UTILS.V_math;
let LC = 0;
```

Lowering patterns observed:

| Source | Emitted JS |
|---|---|
| `bool usingMenu = false;` (global) | `let V_usingMenu = false;` |
| `str[] admins = str["a","b"];` | `let V_admins = ["a", "b"];` |
| `public action render(num delta) {...}` | `function V_render(V_delta) { try { LC = 0; ... } catch (e) {EH(e.message)}; }` |
| `GAME.OVERLAY.drawRect(...)` | `this.GAME.V_OVERLAY.V_drawRect.bind(this)(...)` |
| `obj[] players = GAME.PLAYERS.list();` | `let V_players = this.GAME.V_PLAYERS.V_list.bind(this)();` |
| `for (num i = 0; i < lengthOf players - shift && i < 14; i++)` | `for (let V_i = 0; V_i < ((L(V_players)) - V_playerShifted) && V_i < 14; (V_i)++) { if (LC++ > 1e+6) RE(22,8,"Loop timed out."); ... }` |
| `players[i + shift].username` | `V_players[BC(V_players,V_i + V_playerShifted,29,34)].V_username` |
| `(str) player.username == admins[i]` | `S(V_player.V_username) === V_admins[BC(V_admins,V_i,60,40)]` |
| `x == y` | `x === y` |
| `if (c) {...}` | `if (c) {...};` |
| `usingMenu = !usingMenu;` | `V_usingMenu = !V_usingMenu;` |
| `GAME.NETWORK.send(toStr cmd, {playerSelected: players[sel].id});` | `this.GAME.V_NETWORK.V_send.bind(this)(S(V_commandSelected), {V_playerSelected: V_players[BC(...)].V_id});` |
| `GAME.SCENE.setSkyDome('', '', '', {texture: (num) data.Id});` | `this.GAME.V_SCENE.V_setSkyDome.bind(this)('', '', '', {V_texture: A(V_data.V_Id,F)});` |
| module epilogue | `return {render: V_render.bind(this), onKeyPress: V_onKeyPress.bind(this), ..., start: function(){}, update: function(){}, ...};` |

Consequences KN must reproduce:

* All script-visible property names are stored with `V_` (objects created by scripts, objects sent
  over the network, storage keys passed to callbacks, API option objects). Host code reads options
  as `data["V_<name>"]` (`API:1122` `v13689("canMelee")` = `addPrefix`). In KN the prefix is an
  implementation detail **except** where it is observable: the 2000-character network size limit is
  measured on the JSON **with** `V_` prefixes (2.5), and `GAME.log` prints objects with prefixes
  removed (`KSJ:215-235` `sanitize`).
* `this` inside script code is the globals object `{GAME: prefixedApi}` (`KRT:3-8`), so `GAME` cannot
  be shadowed or reassigned.

---

## 2. Execution model

### 2.1 Client and server scripts: shipping and placement

| | Client script | Server script |
|---|---|---|
| Map JSON key | `scripts.client` | `scripts.server` |
| Runs in | every connected client's `Game` (the rendering game) | the authoritative game (hosted server; KN: the dedicated server **and** the in-process offline server) |
| Sent to clients | yes (inside the map data of the `init` packet) | **never**: the server deletes `scripts.server` from the map data it forwards (`GJS:625-628`) |
| Editor test play | yes (compiled via `ksT`) | **no**: the editor deletes it before testing (`EDT:2196-2213`; template header "Server Script runs only on Hosted server & not in test mode", `SYN:4`) |
| Gated by | `config.scriptingEnabled` (`CFG:914`) | `config.scriptingEnabled && config.serverScripting` (`CFG:914-915`, `GJS:164`) |
| Template | `SYN:3` | `SYN:4` |

* Which script a `Game` loads depends only on whether it has a `serverIO`: `addScripts(scripts,
  serverIO ? "server" : "client")` (`GJS:624`).
* v7 published flow: the server compiles both scripts (`prepareGame`'s `scriptDecoder`), runs the
  server one and ships the compiled client one. KN flow [KN]: the server forwards the client
  **source** (KN has no JS); every client compiles it with the same KN compiler. The server MAY
  pre-validate the client script and refuse to host a map whose client script does not compile
  (error `Client script: <l>:<c> <message>`).
* Custom maps only: official v7.1.4 maps never carry scripts in this tree except the 9 listed in 0.2.

### 2.2 Lifecycle

**Client** [REF]:
1. Every `init` packet (map load **and every round restart**) does `Game.mainScript = null`
   (`IDX:13505`) then `Game.prepareGame(customsData.data, ...)` (`IDX:13509`), which parses the
   client script again: **globals are re-initialised every round**, and global initialisers run.
2. `Game.init(...)` (`IDX:13539`) builds the round; at its end `mainScript.start()` runs (`GJS:3789-3791`).
3. Hooks run until the next `init` or disconnect. `overlay.resetAll()` (`IDX:13493`, `GOV:753-758`)
   clears `#customGUIHolder` (removing every script DIV attached to the default parent) at each `init`.

**Server** [REF]:
1. `prepareGame` once when the map is set (host / custom game / map rotation, `SRV:547`, `SRV:1234`):
   parse, run global initialisers.
2. `Game.init` at every round start (including after `endTimer`, `GJS:4753-4760`) calls `start()` again:
   **globals persist across rounds** on the server.
3. `onGameEnd()` at round end (`GJS:4665-4667`), after `mode.onEnd`.
4. `onServerClosed()` when the game instance is destroyed (`VER:1631`; call site not in tree) [KN: on
   `Game` teardown/map change before the script is freed].

**KN rules** [KN]:
* One `KsScript` instance per (Game, side). Loading a new map, rehosting or leaving a game destroys the
  instance (`VER:2689` "Fixed issue where client scripts continue when game is rehosted").
* On destroy: run `onServerClosed` (server only), stop all script-started sounds, remove script scene
  objects and lights, remove every DIV the script created (any parent), clear overlay state, drop
  pending storage callbacks.
* The offline client process hosts two instances: the sandbox server's `Game` (server script; the real
  server core runs in-process, `server/include/server_sandbox.h`, `client/src/net/offline.c`) and the
  rendering client `Game` (client script). They never share VM state; their messages go over the
  sandbox's loopback connection exactly like online.

### 2.3 Compilation, interpretation and sandboxing

* v7: compiled to JS and run with `new Function` bound to `{GAME: prefix(api)}` (`KSJ:390`, `KRT:3-8`).
  Isolation came from: identifier prefixing (no access to JS globals: `process`/`require` shadowed by
  `const process = {}` in the prelude), the `KCF` blacklist, the prefixed API proxy (every API function
  wrapped, results re-wrapped, `KSJ:237-310`), and the loop counter.
* KN [KN]: a C compiler (lexer -> parser -> checker -> bytecode) and a bytecode VM in `shared/`
  (section 6). Scripts can reach the engine only through the host binding table; there is no
  reflection, no file or network access, no FFI. Limits per 1.14.
* Compile once per load; the compiled program is immutable and may be shared by multiple instances of
  the same map on a server (each instance has its own globals and heap).
* Determinism: `UTILS.randInt`/`randFloat` use the engine RNG (`Math.random` in v7); KN uses a per-script
  PCG stream (`shared/lib/pcg_basic`) seeded from the game seed so offline replays are reproducible.

### 2.4 Tick rates and ordering relative to the game loop

**Server** (`GJS:4728-4740`, `GJS:5176`): `setInterval(tick, config.serverTickRate)` with
`serverTickRate = 1000/30` ms (`CFG:34-37`). Each tick: `delta = now - lastUpdate` (real ms),
`update(delta, now)`, then `syncState()` / `updateState()`. Inside `update` (`GJS:4743-4995`):

1. round/end timers, trigger timers and the delayed `triggerQueue` (trigger actions such as
   **Custom Action** -> `onCustomTrigger` run here, `GJS:4890-4908`);
2. powerups, bots, `players.update` (no script call), sounds, `AI.update`, pets, air drops,
   teleporters, spinners, destructibles, flags, spawnables, dom zones, mystery boxes, krum machines,
   objective time, `mode.update`, zone;
3. `projectiles.update`;
4. **`mainScript.update(delta * config.deltaMlt)`** (`GJS:4960-4962`);
5. (`RENDER.render` on clients).

Player input processing happens when input packets are handled (outside `update`); each processed
input frame calls **`onPlayerUpdate(id, delta, inputs)`** before the frame is applied (`PLJ:894-947`).
`syncState` sends the "l" packet including custom-sync values (2.6).

**Client** (`IDX` frame loop, `requestAnimationFrame`): `gameTimeDiff = min(frame dt, config.dltMx)`
with `dltMx = round(1000/30) = 33` ms (`CFG:40`, `IDX:16690`). Order per frame:

1. `CONTROLS.update`, input frame built, **`tmpPlayer.procInputs(frame)`** -> client `onPlayerUpdate`
   for the local player (`IDX:16842`); in test/sandbox also the sandbox server copy.
2. **`Game.update(gameTimeDiff, ...)`** (`IDX:17003`) -> same `update` body as above minus the server-only
   parts -> **client `update(gameTimeDiff * deltaMlt)`** -> `RENDER.render`.
3. `overlay.render(...)` (HUD canvas), then **`render(gameTimeDiff)`** inside
   `ctx.save(); ctx.scale(overlay.scale, overlay.scale); render(); ctx.restore()` (`IDX:17025-17031`).
   Note: `render` receives the **unscaled** frame delta (no `deltaMlt`).
4. Reconciliation (server "l" packet): replays pending inputs through `procInputs` (`IDX:14782-14791`),
   so client `onPlayerUpdate` **fires again for replayed frames**.

Event hooks (keys, mouse, controller, DIV clicks, network messages, spawn/death) run synchronously
from their event handlers, i.e. between frames.

`GAME.TIME.fixedDelta()` returns the last `delta` given to `Game.update` **before** `deltaMlt`
(`GJS:4745`, `VER:1544`).

**KN mapping** [KN]: server `Game` tick (`shared/src/game.c`, driven by the server core's tick step, also
inside `sandbox_step()` offline) at the KN server tick rate; call the script `update` at the same point
(after projectiles, before snapshot send). Client: call
`update` from the client `Game` update and `render` after the HUD pass with the overlay transform
from 3.1.10 set up.

### 2.5 Networking between client and server scripts

Wire messages in v7 (`IDX:818-824`, `IDX:18150`, `API:1350-1392`):

| Direction | v7 packet | Payload | Delivered to |
|---|---|---|---|
| client -> server | `"sd"` | `(id, data)` | server `onNetworkMessage(id, data, playerID)` (handler not in tree; signature from `SYN:4`) |
| server -> one client | `"sd"` | `(id, data)` | that client's `onNetworkMessage(id, data)` |
| server -> all clients | `"sd"` (broadcast) | `(id, data)` | every client's `onNetworkMessage(id, data)` |
| server -> all clients | `"ksL"` | `(isError, [strings])` | client console `[KRUNKSCRIPT] ...` (`IDX:17769-17771`) |

`GAME.NETWORK.send(id, data, playerID)` / `GAME.NETWORK.broadcast(id, data)` rules (`API:1350-1392`):

* Disabled entirely unless `config.serverScripting` (`false` returned).
* Size check: `JSON.stringify(data).length > 2000` (for objects/arrays; `(data + "").length` for
  primitives) -> returns `false`, nothing sent. Measured on the **prefixed** JSON (keys are `V_x`).
  `JSON.stringify` throwing (cycles) -> `false`.
* `broadcast`: server only; global rate limit **10 calls per 1-second window** (counter key `c_broad`
  reset every 1000 ms, `API:41-45`) -> 11th call returns `false`. Returns `true` when sent.
* `send` on the server: needs `playerID` (empty -> `false`); rate limit **20 per second per target
  player** (`n_send<playerID>`). Returns `true` when sent.
* `send` on the client: sends to the server, **returns `undefined`** in v7 (missing `return`; the docs
  and `VER:2292` say true/false). KN returns `true` when queued, `false` when rejected [KN fix]. No
  client-side rate limit in v7.
* `id` is `str` (not validated in v7). KN: `ToString(id)`, max 64 UTF-8 bytes, else `false` [KN].
* `data`: any JSON-serialisable value (`obj`, arrays, `str`, `num`, `bool`). Delivered as the parsed
  value (an `obj` arrives as `obj`). Action values inside are dropped (JSON). Host objects (player
  wrappers) serialise their getter values (KN: enumerate host properties, skip methods) [REF JSON of
  getters].
* `onNetworkMessage(id, data, playerID)` on the server receives the sender's player id.

Server-side inbound limits (v7's server code is absent; `VER:1890`, `VER:1916`, `VER:2186` only say
limits exist) [KN]: drop client `"sd"` messages beyond **20 per second per player** or larger than
2000 prefixed-JSON characters (silently; no hook call).

**KN wire format** [KN]: new packets in `shared/include/shared.h`:
* `NET_PACKET_SCRIPT_MSG = 120` (both directions): chunked like `NetSocialChunk` because
  `NET_MAX_PAYLOAD` is 512 bytes: `{uint16 msg_seq; uint8 chunk; uint8 chunk_count; bytes}`;
  reassembled body = `uint8 id_len, id bytes, KsValue binary encoding`. Max reassembled 4096 bytes.
* `NET_PACKET_SCRIPT_LOG = 121` (S->C): `{uint8 is_error; char text[480]}`, one line per packet
  (args already joined with spaces, truncated with `...`).
* KsValue binary encoding (`ks_serial.c`): tag byte (`0 undefined, 1 null, 2 false, 3 true, 4 num f64-LE,
  5 str (varint len + UTF-8), 6 array (varint n + values), 7 obj (varint n + (key str, value)*)`).
  Size limit check uses the JSON length rule above, computed by `ks_json_length_prefixed()`.

### 2.6 Persistence, storage and synced values

**GAME.STORAGE** (server only, account-scoped, per map) — `API:2222-2324`, `GJS:1168-1250`:
* Data is a flat map of string keys to string values, stored per (account name, map name). `access`
  `"global"` vs anything else (`"private"`) selects scope `1`/`0` (meaning in the backend: global data is
  readable by other maps of the same creator via `load(account, mapName)`) [REF `GJS:1224-1236`
  passes `customMapData.creator`].
* Write validation (`GJS:1168-1223`): requires a published map (`defaultSetts` in prod: `"Storage only
  supported on published maps"`), map name (`"Missing map data"`), an object (`"Missing storage data"`),
  ≤ 30 keys (`"Too many keys: max 30"`), key ≤ 20 chars after removing `,'"` (`"Key too long"`),
  value `ToString` ≤ 100 chars after removing `,'"` (`"Value too long"`), the player must be connected
  (`"Player not connected"`; looked up by account **name**).
* Operation types (`type` 0..5; trigger "Save Data" options, `TRG:1186`): `0` set, `1` set if higher,
  `2` set if lower, `3` increase by, `4` decrease by, `5` delete key [`VER:1680` "delete storage values
  by key"; exact mapping KN]. `STORAGE.set` uses type 0, `STORAGE.update` type 3.
* Concurrency/rate limits per account name: one in-flight `set`/`update`/`load` per account per
  operation (`"storage.set already in progress for <name>"`), and at most **1 `set` and 1 `update` per
  10 seconds per account** (`"Calling storage.set too much"`) (`API:2226-2233`, 10 s counter `API:46-51`).
* Callbacks run later (asynchronously) as a fresh host entry with `(data obj, success bool, accountName str)`
  (`VER:1550`); `data` maps keys to **string** values.
* `STORAGE.transact` always logs `Transactions disabled for now` (`API:2287-2289`).
* KN backend [KN] (WP6, `server/src/ks_storage.c`, rules shared with kn_brain in `shared/src/ks_storage_data.c`):
  the server forwards storage ops to kn_brain (`doc/backend.md` 7.6 ops `storage` / `storage_load` /
  `map_reward`, table `ks_storage`; the client never talks to the DB). Online, storage needs a lobby hosting a
  published map (`config.customMap`: v7's customMap + defaultSetts); other lobbies get `"Storage only supported
  on published maps"`. A standalone server_bin (no brain) answers v7's no-database `"Error"`. Offline sandbox: one
  JSON document per map, `ks-storage-<fnv1a32 of the map name>` in the client's storage
  (`$XDG_CONFIG_HOME/krunknative/<key>.json`, web localStorage), same validation and semantics; the sandbox player
  counts as an account named `"local"` (accountID `"0"`). Script keys are stored with v7's `V_` prefix (the 20-char
  key limit counts it); trigger keys (Save Data) are not, and both read back un-prefixed. Set / update callbacks get
  the touched keys as stored after the operation; load rows come in key order.

**GAME.COOKIES** (client only, `API:2325-2370`): `localStorage` keys `krkcst_<mapId>_<key>`. Values are
`ToString`ed by `localStorage` (arrays become `"a,b"`). `load` returns `getSavedVal(...) + ""` i.e.
`"null"` when missing (sic). KN: `html_storage_get/set` (`client/include/html_dom.h:120-121`) with the
same key scheme; map id = custom map id or map file name.

**Custom synced player values** (`player.registerSyncValues`, `API:499-548`, `VER:1209`): server-only.
Up to 20 names per player (`custSync`), each ≤ 20 chars. Each server sync (`syncState`, `GJS:5030-5049`)
appends `[name, value, name, value...]` for the **local** player only, read from the player's script
properties; the client writes each value into its own player wrapper **only if** that property already
exists there with a non-object value (`IDX:14769-14775`). So a client script must initialise
`self.myVal = 0;` before synced updates show. The server-side reader (`serverHooks.checkCustSync`) is
not in the tree; KN: for each name, read `player.<name>` from the server player wrapper, send
`num`/`str`/`bool` values only. Lists are cleared when the player resets (`PLJ:473`).
KN wire: append to the existing recon/"l" equivalent (`NET_PACKET_RECON`) as a `KsValue` array
or send `NET_PACKET_SCRIPT_SYNC = 122` at the same cadence [KN].

### 2.7 Permission and security model

| Concern | v7 rule | Source |
|---|---|---|
| Server-only powers | Gated on `isServer && io` (`p17043 && p17044`): loadout/class/score/lives writes, `respawn`, `registerSyncValues`, `NETWORK.broadcast`, `CHAT.*`, `STORAGE.*`, `ADMIN.*`, `PAYMENTS.giveKR`, `AI.spawn/remove/list`, `AI.playAnim`, `TIME.freeze`, `CONFIG.getHost` | `API` passim |
| Client-only powers | Gated on `!isServer` or on `RENDER`/`overlay`/`game.ui` existing: DOM, overlay, scene, camera, inputs, cookies, settings, mods, title, change game | `API` passim |
| DOM ids | 59 protected ids (`API:3`, e.g. `aHolder`, `mapVote`, trade ids, `accName`, `accPass`...) cannot be parents/targets; `getDIVText` also refuses `menuWindow` (`API:4`) | `API:3-4` |
| DOM text | `updateDIVText` refuses text containing (case-insensitive) `pass`, `login`, `user`, `register`, `signin` (`API:5`, `API:1828-1832`) | |
| CSS | `UTILS.cssCheck`: after removing `\ ' " \``, reject if it matches `import|canvas|url|expression|behavior|-moz-binding|javascript|exec` (with `important` allowed) (`UT:1420-1430`) | |
| Title | `setTitle` keeps `[\w\- ]` only (`API:1318-1327`) | |
| Chat | message filtered to `[\w:()/? -]` then `/` removed; ≤ `chatMaxLength` 70; non-blank (`API:1430-1457`, `CFG:2071`) | |
| Settings | `SETTINGS.get/set/list` hide settings of type `url`/`text` and flagged `dev`, `kpd`, `verified`, `noMod` (`API:1848-1882`) | |
| Admin | `kick`/`ban` refuse developer accounts (`account.isDev`) (`API:3231-3267`) | |
| KR | `giveKR` needs `game.krRewards` (host option, only for custom maps, `REF/src/data/customGameConfig.js:438-445`) and amount > 0; 4/s | `API:3089-3097`, `GJS:4341` |
| Rate limits | 1-s counters (`API:16-22`), 10-s counters (`API:23-29`), in-flight locks (`API:7-15`) | see 3.1 tables |
| Assets | asset ids are digits only (`replace(/[^\d]/g,"")`) for images/models; URLs come from `UTILS.userAssetsUrl` | `API:1691`, `API:2818` |

KN keeps all of the above. KN additions [KN]: the VM limits of 1.14; `GAME.MODS.load` only accepts
KN resource-pack URLs the client already allows; `GAME.changeGame` asks the existing client
"play map" flow (no arbitrary URL navigation).

### 2.8 Script console (GAME.log and errors)

* `GAME.log(...args)` -> `console.log(args.map(sanitize))` (`API:1307-1309`); `GAME.log` is not rate limited.
* Formatting: each argument via `logStringify` (`UT:1500`): objects/arrays -> `JSON.stringify(x, null, 2)`
  with `V_` prefixes removed (`sanitize`, `KSJ:215-235`), functions -> omitted inside objects / `null`
  inside arrays; everything else `x + ""`. Joined with one space.
* Client: printed as `[KRUNKSCRIPT] <text>` (cyan; errors on dark red) (`GJS:153-160`).
* Server: `game.broadcast("ksL", isError, strings)` to **every** client (`GJS:142-152`), shown as
  `[KRUNKSCRIPT] <text>` in red (`IDX:17769-17771`).
* KN: client prints to the KN console (`client/include/console.h`) and stderr; server prints to its log and
  sends `NET_PACKET_SCRIPT_LOG` to all clients, max 20 lines per second per game (excess dropped with one
  `... (log rate limited)` line) [KN].

---

## 3. The API surface (`GAME.*`)

### 3.0 Conventions

* Root namespace `GAME` (`config.sysLibName = "GAME"`, `CFG:913`). Aliases: `UTILS` = `GAME.UTILS`,
  `Math` = `GAME.UTILS.math` (`SYN:640-643`).
* **Side**: `C` client script only, `S` server script only, `B` both. "Gated" means the call is a
  silent no-op that returns `undefined` on the other side (no error), exactly as v7.
* **Args**: every API argument is untyped (1.5 rule 6). The "Signature" column gives the documented
  types; "Defaults/validation" gives what v7 does with missing or wrong values. `isNum(x)` means
  `UTILS.isNumber` (`UT:355`: a finite JS number; **strings fail**). `isHex(x)` = `/^#([0-9A-F]{3}){1,2}$/i`
  (`UT:373`). `isStr(x)` = non-empty string (`UT:365`). `clamp(x,a,b)` = `UTILS.limitMM` (`UT:351`).
* **Returns**: script-visible type (`TYP`) and actual value. API results are host values: objects are
  wrapped so that script code reads un-prefixed names (`KSJ:237-310`).
* **Engine** column: v7 object touched -> KN implementation target (file names from section 6).
* Every API function is wrapped: a host exception becomes `Internal Error "<path>"` (`KSJ:295-307`).
* Rate limits (`API:7-51`):

  | Kind | Helper | Window | Keys used |
  |---|---|---|---|
  | count per 1 s, fails when count >= limit | `f2123(key, n)` | counters reset every 1000 ms | `g_change` 3, `c_broad` 10, `n_send<playerID>` 20, `c_send` 3, `c_broadcast` 3, `p_snd2D` 10, `p_snd3D` 10, `p_givKR` 4, `loadmd` 2 |
  | count per 10 s, fails when count > limit | `f2124(key, n)` | reset every 10 000 ms | `str_set_<acct>` 1, `str_upd_<acct>` 1 |
  | in-flight lock | `f2121/f2122` | until callback | `str_set_<acct>`, `str_upd_<acct>`, `str_lod_<acct>` |

  Note `f2123(key, n)` increments first and fails when the new count `>= n`, so `c_send` 3 allows **2**
  calls per second, `c_broad` 10 allows 9, `n_send` 20 allows 19, sounds 9, `p_givKR` 3, `loadmd` 1,
  `g_change` 2. KN must reproduce the off-by-one. Windows are wall-clock fixed windows (not sliding),
  per script instance (per `Game`).

### 3.1 Namespaces

#### 3.1.1 `GAME` root (`API:1307-1327`)

| Signature | Side | Returns | Behaviour | Engine | Example |
|---|---|---|---|---|---|
| `GAME.log(...args)` | B | void | 2.8 | script console / `NET_PACKET_SCRIPT_LOG` | `GAME.log("hp", (str) p.health);` |
| `GAME.changeGame(str mapName)` | C | void | rate 3/s (2 allowed); `escapeStr` then `window.playMap(name)` (joins/hosts that map). Corpus also passes a 2nd arg `{host: true, private: true}` which v7 **ignores** | client "play map" flow (`client/src/net/backend_*.c`) | `GAME.changeGame("eterno_sim");` |
| `GAME.setTitle(str title)` | C | void | keeps `[\w\- ]`, empty -> ignored; sets the window/document title | GLFW window title / `document.title` (web) | `GAME.setTitle("My Game");` |

#### 3.1.2 `GAME.DEFAULT` (`API:1328-1349`)

| Signature | Side | Behaviour | Engine | Example |
|---|---|---|---|---|
| `disablePlayerBehaviour()` | B | `game.noDefaultBehav = true`: `procInputs` skips **all** default movement/rotation/shooting logic after `onPlayerUpdate` (`PLJ:948`) | `Game.ks_no_default_behaviour` checked in `player_process_input` | `GAME.DEFAULT.disablePlayerBehaviour();` |
| `disable3D()` | C (needs RENDER) | `RENDER.disable()`: stop rendering the 3D scene (overlay/UI only) | client renderer skip-scene flag | `GAME.DEFAULT.disable3D();` |
| `disablePrediction()` | B | `game.noPred = true`: client does not run local prediction/reconciliation replays (`IDX:14782`, `IDX:16840`) | client recon | |
| `disableServerSync()` | B | `game.noSync = true`: server stops sending the player-state "l" packet (`GJS:5037`) | `NET_PACKET_RECON` send gate | |

All four are one-way until the script is unloaded (flags live on `Game`; KN resets them when the script instance is destroyed).

#### 3.1.3 `GAME.NETWORK` (`API:1350-1392`) — see 2.5

| Signature | Side | Returns | Example |
|---|---|---|---|
| `send(str id, any data, str playerID = "")` | B (server needs `playerID`) | `bool` | client: `GAME.NETWORK.send("check", {age: 20});` server: `GAME.NETWORK.send("check", {status: "Passed"}, playerID);` |
| `broadcast(str id, any data)` | S | `bool` | `GAME.NETWORK.broadcast("round", {n: 3});` |

#### 3.1.4 `GAME.CHAT` (`API:1430-1457`)

| Signature | Side | Behaviour | Example |
|---|---|---|---|
| `send(str playerID, str message, str color = "#fc03ec")` | S | rate `c_send`; filter (2.7); falsy `playerID` -> broadcast. Sends chat line kind 2 (server message) with colour | `GAME.CHAT.send(id, "Welcome", "#00ff00");` |
| `broadcast(str message, str color = "#fc03ec")` | S | rate `c_broadcast`; filter; to everyone | `GAME.CHAT.broadcast("Round 2");` |

Engine: `game.broadcast("ch", -1, null, msg, 2, color)` -> KN `NET_PACKET_CHAT` with `kind = NET_CHAT_SERVER`;
add a `uint32 color` field (0 = default `#fc03ec`) to `NetChatPacket` [KN].

#### 3.1.5 `GAME.UTILS` and `GAME.UTILS.math` (`API:1458-1546`, helpers in `UT`)

All `B`. Pure functions -> `shared/src/ks/ks_stdlib.c`. `|| 0` means NaN/`-0` -> `0`.

| Signature | Returns | Definition | Example |
|---|---|---|---|
| `math.E`, `math.PI`, `math.PI2` | `num` | constants (`PI2 = 2*PI`) | `Math.PI2` |
| `math.sin cos tan asin acos atan sinh cosh tanh asinh acosh atanh log exp sqrt abs round floor ceil trunc (num x)` | `num` | `Math.<fn>(x) \|\| 0`; JS `Math.round` rounds .5 up (`round(-2.5) = -2`) | `Math.floor(x / 2)` |
| `math.atan2(num y, num x)` | `num` | `Math.atan2(y,x) \|\| 0` | |
| `math.pow(num x, num y)` | `num` | `Math.pow \|\| 0` | |
| `math.min(...num)`, `math.max(...num)`, `math.hypot(...num)` | `num` | variadic; `min()` = `Infinity`, `max()` = `-Infinity` (truthy, kept); `\|\| 0` | `Math.max(0, hue - 10)` |
| `math.lerp(num a, num b, num t)` | `num` | `a + (b - a) * t` (no `\|\|0`) | |
| `math.toRad(num deg)`, `math.toDeg(num rad)` | `num` | `deg*PI/180`, `rad*180/PI` | |
| `math.calcPerc(num n, num perc)` | `num` | `n / 100 * perc` | |
| `math.roundToNearest(num n, num near)` | `num` | non-number n -> 0; `n>0 ? ceil(n/near)*near : n<0 ? floor(n/near)*near : n` (`UT:71`) | |
| `math.roundDecimal(num n, num places = 0)` | `num` | `round(n * 10^p) / 10^p` | `Math.roundDecimal(1.236, 2)` -> 1.24 |
| `getDist2D(x1, y1, x2, y2)` | `num` | Euclidean | |
| `getDist3D(x1, y1, z1, x2, y2, z2)` | `num` | Euclidean (`UT:151`) | |
| `getDir2D(x1, y1, x2, y2)` | `num` | `atan2(y1 - y2, x1 - x2)` (`UT:185`) — direction **from point 2 to point 1** | `UTILS.getDir2D(mx, my, w/2, h/2)` |
| `getDir3D(x1, y1, z1, x2, y2, z2)` | `obj {x, y}` | `getDrs` (`UT:186-194`): `dx=x2-x1, dy=y2-y1, dz=z2-z1`; `y = -atan2(dy, hypot(dx,dz))`; `x = -atan2(-dx, dz) + PI/2` (read-only getters) | |
| `anglDst(num a, num b)` | `num` | `atan2(sin(b-a), cos(a-b))` signed shortest angle | |
| `randInt(num min, num max)` | `num` | `floor(rand*(max-min+1))+min` inclusive | `UTILS.randInt(0, 360)` |
| `randFloat(num min, num max)` | `num` | `rand*(max-min)+min` | |
| `hexFromHue(num hue)` | `str` | `UT:661-666` (HSV with S=V=1, `rgbToHex`); fallback `"#ffffff"` | `UTILS.hexFromHue(120)` -> `"#00ff00"` |
| `invertHex(str hex)` | `str` | `"#" + (0x1RRGGBB ^ 0xFFFFFF).toString(16).substr(1).toUpperCase()`; garbage in -> garbage/`"#ffffff"` | |
| `RGBtoHEX(num r, num g, num b)` | `str` | non-numbers -> 0; `"#" + (r<<16\|g<<8\|b)` hex lowercase, zero-padded to 6 | |
| `HEXtoRGB(str hex)` | `obj {r, g, b}` | invalid -> `#ffffff`; expands `#abc` | |
| `toLower(str)`, `toUpper(str)` | `str` | ASCII+Unicode case mapping of `ToString(x)` (`""` for falsy) | |
| `truncateTxt(str text, num len, bool noDots = false, num start = 0)` | `str` | `start = abs(isNum(start) ? start : 0)`; if `len` not a number -> text; if `text.length > len`: `text.substring(start, len)` + (`noDots` ? `""` : `"..."`); else text (`UT:218`) | `UTILS.truncateTxt(t, i, true)` |
| `replaceText(str text, str find, str with)` | `str` | **first** occurrence only (`String.replace` with a string pattern; `$&`, `$1` style patterns in `with` are interpreted by JS — KN implements `$$`, `$&`, `` $` ``, `$'`) | |
| `textContains(str text, str find)` | `bool` | substring test | `UTILS.textContains(id, "key")` |
| `copyToClipboard(str text)` | void | **client only**: copies `ToString(text)` to the OS clipboard (`html_clipboard_write`) | `UTILS.copyToClipboard(code);` |

#### 3.1.6 `GAME.RAYCAST` (`API:1547-1557`)

| Signature | Side | v7 behaviour | KN |
|---|---|---|---|
| `from(num x, num y, num z, num yaw, num pitch, num dist)` | B | **broken**: references an uninitialised vector and throws -> `Internal Error "RAYCAST.from"`; the live game later made it an empty stub | Return `undefined` silently [KN; matches the live stub]. A future KN extension may return `{hit, distance, x, y, z}` |
| `fromPlayer(obj player, num dist)` | B | no-op, `undefined` | same |

#### 3.1.7 `GAME.TIME` (`API:1558-1580`)

| Signature | Side | Returns | Behaviour | Example |
|---|---|---|---|---|
| `now()` | B | `num` | `Date.now()` (Unix ms, wall clock) | `num t = GAME.TIME.now();` |
| `fixedDelta()` | B | `num` | `game.fixedDelta`: last raw update delta (ms, before `deltaMlt`) | |
| `getReadable(num ms)` | B | `str` | `getReadableTime(ms)` with no flags (`UT:280-292`): `"[Ny ][Nd ][Nh ]Mm "` — note seconds are **omitted** and a trailing space remains (`getReadable(61000)` = `"1m "`) | |
| `freeze()` | S | void | if round timer is running (not `"skip"`, not ending): save `gameTimer`, set it to 0 (pauses round clock) | `GAME.TIME.freeze();` |
| `unfreeze()` | S | void | restores the saved timer | |

#### 3.1.8 `GAME.INPUTS` (`API:1581-1655`)

| Signature | Side | Returns | Behaviour | Engine |
|---|---|---|---|---|
| `keyDown(num keyCode)` | C | `bool` | `controls.keys[keyCode]` (legacy DOM `keyCode`, table 4.3); non-number -> `undefined` | client input state |
| `mousePos()` | C | `obj {x, y}` | last mouse position in window CSS px (`RENDER.tmpMouse`, set on mousemove, `CTL:37`) | |
| `mouseMovement()` | C | `obj {x, y}` | last mousemove `movementX/Y` (raw pixels of the latest event; not accumulated) (`CTL:38`) | |
| `mousePosOverlay()` | C | `obj {x, y}` | mouse position in overlay units: `canvas.width/scale * (mx/innerWidth)` (same space as `OVERLAY` drawing) | |
| `freeMouse()` | C | void | `controls.forceFreeCam = true` + exit pointer lock: cursor free for the rest of the script's life | |
| `unlockMouse()` | C | void | temporary unlock: controls disabled, keys reset, pointer lock released; a mousedown on the game view **re-locks** (`CTL:585-589`); clicks on script DIVs do not (they stop propagation) | |
| `lockMouse()` | C | void | clear temporary unlock and re-lock | |
| `disableDefault()` | C | void | `game.dontSendInputs = true`: client stops predicting/sending input frames (`IDX:16840-16847`) | |
| `enableDefault()` | C | void | undo | |

Key hooks (`onKeyPress` etc.) only fire while controls are enabled (`CTL:125`): after `unlockMouse()`
key hooks stop until the mouse is locked again (Escape excepted) [REF].

#### 3.1.9 `GAME.UI` (`API:1656-1847`) — DOM elements over the game

Parents: `parentId` missing/empty/`"game"` -> `#customGUIHolder` (inside the in-game UI); `"menu"` -> `#MENUDIV`;
any other existing element id that is not protected (2.7). In KN these resolve in the HTML engine page
(`ui/index.html`, which must contain `customGUIHolder`, `MENUDIV`, `inGameUI`) via `html_dom_by_id`.
Every created element gets `pointer-events: all` and a click listener that calls `onDIVClicked(event.target.id)`
and stops propagation (`API:1700-1706`, `API:1731-1737`).

| Signature | Side | Returns | Behaviour | Example |
|---|---|---|---|---|
| `addDIV(str id, bool visible, str style = "", str parentId = "game")` | C | `str` id, or `undefined` on failure | fails if parent missing/protected, `id` exists or is protected, or `cssCheck(style)` fails; `display = visible ? "" : "none"`; `style.cssText += style` | `GAME.UI.addDIV("info", true, "color:#fff;position:absolute;top:90%;");` |
| `addImage(str assetID, str id, bool visible, str style = "", str parentId = "game")` | C | `str` / `undefined` | as `addDIV` but an `<img>` whose `src` = user asset `<digits>/texture.png` (non-digits stripped; empty -> fail); broken image -> `./img/noimg.png` | `GAME.UI.addImage("13075", "logo", true, "width:64px;");` |
| `moveDIV(str id, str parentId, str newId = "")` | C | `str` final id / `undefined` | re-parent (`UTILS.moveElement`), optionally rename; protected ids refused | `GAME.UI.moveDIV("logo", "menu");` |
| `removeDIV(str id)` | C | void | remove element (not protected) | |
| `updateDIV(str id, str cssProp, str value)` | C | void | `el.style[cssProp] = value`; `cssProp` must pass `cssCheck`; a non-string or failing `value` becomes `""` (clears). KN accepts kebab-case (`background-color`) and camelCase (`backgroundColor`) property names | `GAME.UI.updateDIV("info", "display", "none");` |
| `updateDIVText(str id, str text)` | C | void | `textContent = text` (non-string -> `""`); refused if text contains `pass`/`login`/`user`/`register`/`signin` | `GAME.UI.updateDIVText("info", "Press G");` |
| `getDIVText(str id)` | C | `str` / `undefined` | `textContent` | |
| `getProp(str id, str cssProp)` | C | `str` / `undefined` | inline `style[prop] \|\| ""` (`html_dom_get_style`) | |
| `getSize()` | C | `obj {width, height}` | window inner size (CSS px); server `undefined` | |
| `hideDefault()` | C | void | hides the default game UI (`gameUIHider`, `fullMenHider`), `game.ui.hidden = true` | |
| `hideCrosshair()` | C | void | `overlay.disableCrosshair = true` | |

Map-defined GUI text elements (map JSON GUI entries with `t == 3`) are created as `#cusDIV<i>` with the
same click -> `onDIVClicked` wiring (`GOV:761-795`).

#### 3.1.10 `GAME.OVERLAY` (`API:1883-2221`) — 2D canvas over the game

The overlay is a full-window canvas. `scale = overlay.scale` (the UI scale). During `render` the context
is pre-scaled by `scale` (`IDX:17026-17030`), so all script coordinates are in **overlay units**:
width = `canvas.width / scale`, height = `canvas.height / scale` (`OVERLAY.getSize()`). The canvas is
cleared by the engine every frame before the HUD is drawn (`overlay.render`), so scripts redraw every
`render` call. Calls made outside `render` draw with whatever transform is current (unscaled) and are
erased at the next frame [REF behaviour, KN: same].

Colours must be hex (`isHex`), otherwise the default is used. Angles in `drawRect/drawCircle/drawText/
drawImage` are **degrees**; path functions use radians (canvas API).

| Signature | Side | Behaviour (defaults) | Example |
|---|---|---|---|
| `getSize()` | C | `obj {width, height}` in overlay units | `obj s = GAME.OVERLAY.getSize();` |
| `clear()` | C | `clearRect(0,0,width,height)` (current transform) | |
| `offset(num x, num y)` | C | `ctx.translate(x, y)` (persists until restore / next frame) | camera follow in the io template |
| `scale(num s)` | C | `ctx.scale(s, s)` | |
| `measureText(num size = 1, str text = "", str font = "GameFont")` | C | `num` width of `text` at `size`px in `font` | |
| `drawRect(num x=0, num y=0, num w=1, num h=1, num rotDeg=0, str color="#FFF", num opacity=1, bool center=false)` | C | save; alpha; translate(x,y); rotate; `rect(center ? -w/2 : 0, center ? -h/2 : 0, w, h)`; fill; restore | `GAME.OVERLAY.drawRect(0,0,w,h,0,"#000",1);` |
| `drawCircle(num x=0, num y=0, num rx=1, num ry=1, num rotDeg=0, str color="#FFF", num opacity=1, bool center=false)` | C | ellipse **radii** `rx, ry` at `(center ? -rx/2 : 0, center ? -ry/2 : 0)` (sic: the "center" offset uses half the radius) | `GAME.OVERLAY.drawCircle(x, y, 80, 80, 0, "#ff0000", 1);` |
| `drawLine(num x1=0, num y1=0, num x2=0, num y2=0, num thickness=0, str color="#FFF", num opacity=1)` | C | stroked line. Canvas ignores a non-positive `lineWidth`, so `thickness <= 0` keeps the current line width (1 after `save`, unless the script changed it) | |
| `drawText(str text="", num x=0, num y=0, num rotDeg=0, num size=1, str align=null, str color="#FFF", num opacity=1, str font="GameFont")` | C | `font = (size\|\|10)+"px "+font`, `textBaseline = "middle"`, `textAlign = align` if given (`left/center/right/start/end`) | `GAME.OVERLAY.drawText("1", x, y, 0, 70, "center", "#fff", 1);` |
| `drawImage(num assetID=0, num x=0, num y=0, num w=1, num h=1, num rotDeg=0, num opacity=1)` | C | user asset `<id>/texture.png`, drawn **centred** on (x,y); skipped until loaded (async load, cached) | |
| `beginPath() closePath() fill() stroke() save() restore()` | C | canvas state/path ops | |
| `translate(x=0, y=0)`, `transform(a,b,c,d,e,f)`, `setTransform(a,b,c,d,e,f)` | C | canvas transforms (all default 0; `setTransform` with defaults makes a degenerate matrix — same as v7) | |
| `moveTo(x,y)`, `lineTo(x,y)`, `arc(x,y,r,start,end,ccw=false)`, `arcTo(x1,y1,x2,y2,r)`, `ellipse(x,y,rx,ry,rot,start,end,ccw=false)`, `quadraticCurveTo(cpx,cpy,x,y)`, `bezierCurveTo(c1x,c1y,c2x,c2y,x,y)` | C | path building; radii `abs()`ed | `GAME.OVERLAY.arc(50,50,20,0,Math.PI2);` |
| `fillStyle(str color="#FFF")`, `strokeStyle(str color="#FFF")` | C | hex only | |
| `lineWidth(num w=0)` | C | | |
| `lineDashOffset(num off=0)` | C | **sets `lineWidth`** (v7 bug) — KN reproduces | |
| `lineJoin(str j)` | C | `bevel`/`round`/`miter` only | |
| `strokeText(str text, num x, num y, num maxWidth?)` | C | uses current font/stroke style | |
| `strokeRect(x, y, w, h)` | C | | |
| `globalAlpha(num a=0)` | C | | |

KN engine: new `client/src/ks/canvas2d.c` (section 6.4) rendered after the HUD with the UI ortho
projection; `GameFont` = the KN HUD font.

#### 3.1.11 `GAME.SETTINGS` (`API:1848-1882`)

| Signature | Side | Returns | Behaviour | Example |
|---|---|---|---|---|
| `set(str key, any value)` | C | void | `settings.set(key, value, ...)` for allowed settings (2.7), like the user changing it (persisted!) | `GAME.SETTINGS.set("lighting", 2);` |
| `get(str key)` | C | `str` / `undefined` | `ToString(current value)` | `str fov = GAME.SETTINGS.get("fov");` |
| `list()` | C | array of `str` (typed `str`) | allowed keys | |

KN: map v7 setting keys to `client/include/client_settings.h` entries; unknown keys -> `undefined`. A
mapping table (`ks_settings_map.c`) lists every v7 key KN supports (`fov`, `sensitivity`, `scaleUI`,
`showUI`, `lighting`, `shadows`, `resolution`, `volume`...) [KN].

#### 3.1.12 `GAME.STORAGE` (`API:2222-2324`) — see 2.6

| Signature | Side | Behaviour | Example |
|---|---|---|---|
| `set(str accountName, obj data, str access = "private", action callback)` | S | type 0; callback `(obj data, bool success, str accountName)` | `GAME.STORAGE.set(p.accountName, {coins: 5}, "private", saved);` |
| `update(str accountName, obj data, str access = "private", action callback)` | S | type 3 (increment numeric values) | |
| `load(str accountName, str mapName = "", action callback)` | S | callback **required** (else `Missing callback`); `""` = this map; no rows -> logs `No data` and the callback is **not** called | `GAME.STORAGE.load(p.accountName, "", loaded);` |
| `transact(...)` | S | logs `Transactions disabled for now` | |

Callback data keys are the stored keys; values are strings. On error the error text is logged
(`f2125`) and the callback receives `({}, false, accountName)` (set/update) [REF].

#### 3.1.13 `GAME.COOKIES` (`API:2325-2370`) — see 2.6

| Signature | Side | Returns | Example |
|---|---|---|---|
| `save(str key, str\|num\|bool\|array value)` | C | void (objects refused) | `GAME.COOKIES.save("best", (str) score);` |
| `load(str key)` | C | `str` (`"null"` if missing; `""` on server or empty key) | |
| `has(str key)` | C | `bool` | |
| `remove(str key)` | C | void | |

(v7 bug kept: the `bool` check is `typeof value == Boolean`, always false, so **booleans are refused**.)

#### 3.1.14 `GAME.SOUND` (`API:2371-2422`)

| Signature | Side | Returns | Behaviour | Example |
|---|---|---|---|---|
| `play2D(num\|str assetID, num volume = 1, num rate = 0, bool loop = false)` | C | `obj` sound or `undefined` | rate `p_snd2D`; `parseInt(assetID)` (strings accepted, `VER:1157`); plays user-asset sound on the "custom" volume channel (`volumes[6]`); `rate 0` = engine default 1 | `obj s = GAME.SOUND.play2D(12345, 0.5);` |
| `play3D(num\|str assetID, num volume = 1, num x = 0, num y = 0, num z = 0, num rate = 0, bool loop = false)` | C | `obj` sound | positional (Howler 3D): v7 calls `play3D(id, x, y, z, vol, rate, 25, 1, true, 0)` = rolloff factor 25, reference distance 1, custom-asset flag (same argument order as `game.playerSound`, `GJS:687-689`) | |
| `stop(num assetID = 0)` | C | void | stop every playing instance of that asset | |

Sound object (3.2.3). KN engine: `client/src/audio.c` (miniaudio) with user-asset sound loading.

#### 3.1.15 `GAME.ANIM` (`API:2423-2426`)

`playClip(obj, str clipID, num reps)`, `stopClip(obj, str clipID)`: **empty stubs in v7.1.4** -> KN no-ops.
Use `obj.playAnim` / `obj.stopAnim` instead.

#### 3.1.16 `GAME.CAMERA` (`API:2427-2580`)

| Signature / property | Side | Behaviour | Example |
|---|---|---|---|
| `envZone` (read-only `num`) | C | id of the environment zone the camera is in, `-1` none | |
| `detach()` / `attach()` | C | `game.detachedCam = true/false`: when detached the engine stops driving the camera from the player (`IDX:16711-16900`) | `GAME.CAMERA.detach();` |
| `move(num x=0, num y=0, num z=0)` | C | sets the camera rig (yaw object) **position** absolutely | `GAME.CAMERA.move(0, 40, 90);` |
| `rotate(num yaw=0, num pitch=0, num roll=0)` | C | sets the inner camera rotation: `rotation.y = yaw`, `.x = pitch`, `.z = roll` (radians) (`CTL:1001-1005`) | `GAME.CAMERA.rotate(0, 0, 0);` |
| `lookAt(num x=0, num y=0, num z=0)` | C | yaw/pitch of the rig toward the point; resets inner camera offset | |
| `shake(num amount=0)` | C | `RENDER.shake(amount)` | |
| `fov(num deg=0)` | C | sets camera FOV directly | |
| `getObj()` | C | `obj {id}`: the camera as an attach target for `obj.attachTo` | |
| `position.x/.y/.z` (rw) | C | camera rig position | `GAME.CAMERA.position.y = 50;` |
| `rotation.x/.y/.z` (rw) | C | camera rig rotation | |

KN: `client/src/camera.c` (`Camera`), with a `ks_camera_detached` flag consulted where the client sets the
camera from the player each frame.

#### 3.1.17 `GAME.SCENE` (`API:2581-3088`) — client-side visual objects

All `C` (no-ops on the server). Objects created here are **local and visual only**: no collision, not
networked, removed when the script unloads. `aid` = texture asset id (number or string; a `v` in the
string selects a video texture), `colr` = hex colour (default `#FFF`), `data` = option object (3.3.1).

| Signature | Returns | Notes | Example |
|---|---|---|---|
| `addCube(aid, colr, x=0, y=0, z=0, w=1, h=1, l=1, data={})` | `obj` obj3D | box mesh placed with `RENDER.moveMesh(x,y,z)` (the map-object placement convention), scale (w,h,l) | `GAME.SCENE.addCube("", "#f00", 0, 0, 0, 5, 5, 5);` |
| `addPlane(aid, colr, x=0, y=0, z=0, w=1, l=1, data={})` | obj3D | double-sided by default; `mimicSprite` option | |
| `addSphere(aid, colr, x=0, y=0, z=0, w=1, h=1, l=1, data={})` | obj3D | centre at `y + h/2` | `GAME.SCENE.addSphere('', color, d, 12, 0, 5, 5, 5)` |
| `addSprite(aid, colr, x=0, y=0, z=0, w=1, h=1, l=1, data={})` | obj3D | camera-facing sprite, scale `(w, l)` | |
| `addCustom(aid, colr, num[] verts, x=0, y=0, z=0, w=1, h=1, l=1, data={})` | obj3D | triangle list from flat `[x,y,z,...]` (non-numbers filtered), vertices merged, colourised | Custom Geometry template |
| `addAsset(str assetID, x=0, y=0, z=0, scale=1, colr="#FFF", data={}, action onLoad)` | obj3D | model asset; `scale` clamped 0.01..100; flags letters in `assetID` (3.3.4); y offset `+scale` for OBJ models, `+rand(-0.01,0.01)` | `GAME.SCENE.addAsset("13075", 0, 0, 0, 1);` |
| `addSign(x=0, y=0, z=0, w=1, l=1, str text="Hello World", data={})` | obj3D | text plane (3.3.2) | |
| `addLightCone(colr, x=0, y=0, z=0, w=1, l=1, h=1, data={})` | obj3D | additive cone (`fullCone`, `sides`, `thetaLength`) | |
| `addLightBar(colr, x=0, y=0, z=0, w=1, l=1, data={})` | obj3D | additive textured plane | |
| `addPointLight(colr="#f2f8fc", x=0, y=0, z=0, dist=0 (0..1000), decay=1 (0.1..2), intensity=1 (0..15), bool shadow=false)` | obj3D / `undefined` | only when the user's lighting setting >= 2 | `GAME.SCENE.addPointLight(color, 18, 12, 0, 100, 1);` |
| `addSpotLight(colr, x, y, z, tx, ty, tz, dist=0, decay=1, intensity=1, angleDeg=45 (1..90), penumbra=0 (0..1), bool shadow=false)` | obj3D | target at (tx,ty,tz) | Flashlight template |
| `addDirLight(colr, x, y, z, tx, ty, tz, intensity=1, bool shadow=false)` | obj3D / `undefined` | only when lighting == 3 | |
| `addRectLight(colr, x, y, z, w=1, l=1, intensity=1)` | obj3D / `undefined` | only when lighting == 3 | |
| `posToScreen(num x=0, num y=0, num z=0)` | `obj {x, y, onScreen}` | projects a world point to **overlay units**; `onScreen` = inside the view frustum | Converting 3D to 2D template |
| `movObj(obj o, dx, dy, dz)` | void | **adds** to `o.position` | |
| `rotObj(obj o, x, y, z)` | void | sets `o.rotation` | |
| `scaleObj(obj o, x, y, z)` | void | sets `o.scale` | |
| `clear()` | void | removes **every** object from the render scene (map included) | |
| `setFog(str colr, num dist)` | void | dist clamped 10..15000; linear fog near 1 | |
| `setAmbientLight(str colr, num intensity)` | void | intensity clamped 0.1..3 | |
| `setSkyLight(str colr, num intensity, num sunAngX=90, num sunAngY=54, num dist=500)` | void | angles in degrees 0..360 (`CFG:2027-2035`) | |
| `setSkyColor(str colr = "#dce8ed")` | void | clear colour | |
| `setSkyDome(str c0, str c1, str c2, obj data)` | void | gradient sky dome; `data.texture`, `emissiveTexture`, `emissive`, `textureMoveSpeed`, `textureMoveAxis` | `GAME.SCENE.setSkyDome('', '', '', {texture: 123});` |
| `removeSkyDome()` | void | | |

KN engine: `client/src/ks/ks_scene.c` creating `Mesh`es in the client `Scene` (`client/include/client.h`) with
`BasicMaterial`; lights map to the KN lighting model where supported (point/spot lights may be emulated or
ignored with a one-time warning until the renderer supports them) [KN].

#### 3.1.18 `GAME.PLAYERS` (`API:3100-3141`)

| Signature | Side | Returns | Behaviour | Example |
|---|---|---|---|---|
| `list()` | B | `obj[]` | every player in `game.players.list` (bots included), as player objects | `obj[] ps = GAME.PLAYERS.list();` |
| `findByID(str id)` | B | `obj` / `null` | loose compare `player.id == id` (KN: compare as strings) | `obj p = GAME.PLAYERS.findByID(id);` |
| `getSelf()` | C | `obj` / `null` | the local player (`null` before spawn/while spectating; always `null` on the server) | |
| `toggleLOD(bool on)` | C | void | player model LOD on/off | |
| `disableMeshes()` | C | void | do not render player models | |

#### 3.1.19 `GAME.AI` (`API:3142-3185`)

| Signature | Side | Returns | Behaviour | Example |
|---|---|---|---|---|
| `spawn(str assetID, str name, num x, num y, num z, obj data={})` | S | AI object | `game.AI.spawn` with config from `data` (3.3.3); limit `config.aiLimit` = 40 (`CFG:2328`) -> `undefined` beyond | `obj a = GAME.AI.spawn("123", "Zombie", 0, 0, 0, {health: 100, canMelee: true});` |
| `remove(num sid)` | S | void | remove fully | |
| `list()` | S | `obj[]` (client: `undefined`) | | |

KN engine: `shared/src/ai.c` (already ports v3.1.1 AI; extend with the v7 fields).

#### 3.1.20 `GAME.OBJECTS` (`API:3190-3211`)

| Signature | Side | Returns | Example |
|---|---|---|---|
| `list()` | B | `obj[]` of map objects `{sid, position{x,y,z}, scale{x,y,z}}` (read-only; scale = width/height/length) | |
| `getPathNodes()` | B | `obj[]` of `{id, x, y, z}` (AI path nodes) | |

#### 3.1.21 `GAME.TRIGGERS` (`API:3186-3189`)

`list()`, `execute(index, ...args)`: **empty stubs** in v7.1.4 -> KN no-ops returning `undefined`.

#### 3.1.22 `GAME.PAYMENTS` (`API:3089-3099`)

| Signature | Side | Behaviour |
|---|---|---|
| `giveKR(obj\|str player, num amount)` | S | requires host option KR rewards; amount > 0; rate `p_givKR`; `game.rewardKR` (`GJS:4341-4360`). In v7 `rewardKR` expects the **engine player** object; KN accepts a player object or a player id. KN has no KR economy in offline mode -> no-op offline; online forwards to kn_brain [KN] |
| `charge()` | — | empty stub |

#### 3.1.23 `GAME.ADMIN` (`API:3231-3267`)

| Signature | Side | Behaviour |
|---|---|---|
| `kick(str playerID)` | S | not for developer accounts; marks kicked, sends error `"Kicked by host"`, closes the socket (KN: `NET_PACKET_ERROR` + disconnect) |
| `ban(str playerID)` | S | as kick with `"Banned"` and the IP added to the game's ban list (for this game instance) |

#### 3.1.24 `GAME.PLATFORM` (`API:3268-3278`)

| Signature | Side | v7 | KN |
|---|---|---|---|
| `isClient()` | C | desktop client (`window.isDeskCli`) | `true` in the native build |
| `isBrowser()` | C | browser, not mobile | `true` in the Emscripten build (non-mobile) |
| `isMobile()` | C | mobile controls active | `ui_mobile.h` state |

All three return `false` on the server.

#### 3.1.25 `GAME.CONFIG` (`API:3279-3305`)

| Signature | Side | Behaviour |
|---|---|---|
| `getHost()` | S | player object of the host (`game.host`) or `null` (`VER:1147`) |
| `getMatch()`, `getSettings()`, `getWeapons()`, `getClasses()` | — | empty stubs -> `undefined` |

#### 3.1.26 Other namespaces

| Namespace | Signature | Side | v7 | KN |
|---|---|---|---|---|
| `GAME.ADS` | `playVideo()` | C | `window.forceVideoAd()` -> later server `onAdFinished` | no ads: no-op; `onAdFinished` never fires |
| `GAME.MODS` | `load(str url)` | C | `loadModPack(url, true)`, rate `loadmd` | load a KN resource pack if allowed (`ui_mods.h`), else no-op |
| `GAME.MODS` | `reset()` | C | reset mods window | reset to no pack |
| `GAME.URLS` | `openDiscord() openTwitter() openYoutube() openOpensea()` | — | empty stubs | no-ops |
| `GAME.NFT` | `hasWallet(str playerID)` | B | account has a linked wallet | always `false` |
| `GAME.NFT` | `ownedAssets(str playerID, str collection, action cb)` | S | async `cb(bool ok, obj[] nfts)` | logs `Missing wallet or player`, no callback |

### 3.2 Host object types

Each wrapper is created once per engine object and cached (`_protected ||= {...}`), so script-added
properties survive between fetches. Field writes coerce: `parseFloat(v) || 0` for numbers, `!!v` for
booleans, unless noted.

#### 3.2.1 Player object (`API:53-553`) — returned by `PLAYERS.*`, `CONFIG.getHost`

| Member | R/W | Side for writes | Meaning (v7 field) |
|---|---|---|---|
| `id` | R | | player id (`str` in v7; KN: decimal string of the KN player id) |
| `sid` | R | | short numeric id |
| `username` | R | | display name |
| `isYou` | R | | local player (client) |
| `active` | R | | alive/in game |
| `accountID`, `accountName` | R | | account id / name; `""` on the client and for guests (server only) |
| `team` | R | | team number (0 = none) |
| `classIndex` | RW | S | write: valid class index -> change class and refresh items |
| `loadoutIndex` | RW | S | write: swap to that loadout slot if present |
| `ammo` | R | | ammo in current weapon |
| `score` | RW | S | write: `players.score(p, int, 0, true)` (sets) |
| `lives` | RW | S | write: `max(0,int)`; setting 0 when > 0 kills the player |
| `health` | RW | B | write: `setHealth`; if it reaches <= 0 the player is killed |
| `position.x/y/z` | RW | B | feet position (Krunker units) |
| `velocity.x/y/z` | RW | B | units/ms |
| `rotation.x` / `rotation.y` | RW | B | `xDire` (yaw) / `yDire` (pitch), radians |
| `onWall`, `onGround` (`onGround \|\| onRamp`), `onTerrain`, `isCrouching` (`crouchVal >= 1`) | RW | B | movement state |
| `defaultMovement`, `defaultVelocity`, `defaultRotation` | RW | B | default `true`; false disables that part of default movement (`PLJ:969`, `PLJ:1127-1264`) |
| `disableShooting`, `disableMelee` | RW | B | block firing / melee (`PLS:2236`, `PLS:2657`) |
| `visible` | RW | C | player model visibility |
| `inView` | R | C | player visible on screen |
| `assetID` | RW | B | custom player model asset; write -> `players.updateAssetID` |
| `disableDefault(str input)` | method | B | permanently zero one input before default processing: `mouseY mouseX movDir lMouse rMouse jump reload crouch scroll swap restK inter` (`PLJ:911-946`). Reset when the player resets (`PLJ:461`) |
| `getAsset()` | method | C | obj3D of the player model (`meshRef.children[0]`) or `undefined` |
| `playAnim(str clip, num reps = 1)`, `stopAnim()` | method | C | model animation clips |
| `changePrimary(num weaponID)`, `changeSecondary(num weaponID)`, `giveWeapon(num weaponID)`, `removePrimary()`, `removeSecondary()`, `removeMelee()`, `clearLoadout()` | method | S | loadout edits, broadcast as `inat` (KN `NET_PACKET_LOADOUT`); rules in `API:210-397` (weapon 12 = knife) |
| `registerSyncValues(str\|str[] names)`, `deregisterSyncValues(str\|str[] names)` | method | S | 2.6 |
| `respawn()` | method | S | lives <= 0 -> 1; if dead, force respawn (`frs`) |

#### 3.2.2 AI object (`API:554-625`)

| Member | R/W | Notes |
|---|---|---|
| `sid` | R | |
| `displayName` | R | AI name |
| `position.x/y/z` | RW | |
| `rotation.x/y` | RW | v7 bug: reads an undefined variable -> `Internal Error`. KN maps to the AI's yaw/pitch [KN fix] |
| `pathIndex` | R | (its setter is shadowed by a duplicate `behaviour` setter in v7: read-only) |
| `behaviour` | RW | behaviour type |
| `health` | RW | `changeHealth` |
| `playAnim(str clip, num reps = 1)` | method (S) | broadcast animation |

#### 3.2.3 Sound object (`API:626-666`) — from `SOUND.play2D/3D`

`volume` (RW, multiplied by the custom-sound volume setting), `rate` (RW), `fade(from=0, to=1, ms=0)`,
`mute()`, `unmute()`, `play()`, `stop()`, `pause()`.

#### 3.2.4 Scene object "obj3D" (`API:667-953`) — from `SCENE.add*`, `player.getAsset()`

| Member | Notes |
|---|---|
| `id` (R) | render object id (attach target) |
| `visible` (RW) | |
| `emissive` (W, hex) | rebuilds the material with an emissive colour; reads always `""` |
| `opacity` (RW 0..1) | transparent when != 1; models: every sub-material |
| `texture` (RW num asset id) | non-model objects only |
| `animSpeed` (RW) | models: mixer time scale |
| `position.x/y/z`, `rotation.x/y/z`, `scale.x/y/z` (RW) | |
| `move(x, y, z)` | sets position (absolute) |
| `rotate(x, y, z)` | sets rotation (absolute, radians) |
| `rotateLocal(x, y, z)` | rotates about the bounding-box centre on world axes, in x, y, z order |
| `lookAt(x, y, z)` | lights: moves the target; meshes: orient toward the point |
| `playAnim(str clip, num reps = 1)`, `stopAnim()` | animated models |
| `getBone(str name)` | bone object (3.2.5) or `undefined` |
| `updateMorph(str name, num value = 0)` | morph target weight |
| `delete()` | remove from the scene |
| `attachTo(obj target, num x=0, num y=0, num z=0, bool keepWorld=false)` | parent under a pivot at offset (x,y,z) of a player (if `target.sid` exists) or of a scene object/bone/camera found by `target.id` |
| `detach(bool keepWorld=false)` | back to the scene root |

#### 3.2.5 Bone object (`API:954-1027`)

`id`, `move(x,y,z)`, `position.*`, `rotation.*`, `scale.*` (all RW).

#### 3.2.6 Map object (`API:1028-1056`), path node (`API:1083-1098`), NFT (`API:1099-1118`)

* Map object: `sid`, `position.{x,y,z}`, `scale.{x,y,z}` (= width, height, length), read-only.
* Path node: `id`, `x`, `y`, `z`, read-only.
* NFT: `name`, `id`, `tokenID`, `contractAddress`, `collection`, `openListing()` (stub).

#### 3.2.7 Plain result objects

`{width, height}` (sizes), `{x, y}` (mouse, `getDir3D`), `{x, y, onScreen}` (`posToScreen`), `{r, g, b}`
(`HEXtoRGB`), `{id}` (`CAMERA.getObj`), the `inputs` object of `onPlayerUpdate` (4.2). These are ordinary
script objects (writable, no effect on the engine).

### 3.3 Option objects passed into the API

Keys are read with the `V_` prefix in v7; scripts write them plainly.

#### 3.3.1 Scene object options (`API:1202-1284`, `vO445.obj(target, data, kind)`; kind 1 = plane/sprite-like, 2 = asset, 3 = cone)

| Key | Kinds | Effect |
|---|---|---|
| `textureStretching` (bool) | not 2 | stretch instead of tile (`scale = 0`) |
| `textureScale` (num) | not 2 | |
| `textureMoveSpeed` (num) | not 2 | `/10000` per ms |
| `textureMoveAxis` (`"x"`, `"y"`, 0, 1) | not 2 | |
| `textureOffsetX`, `textureOffsetY` (0..1) | not 2 | |
| `textureRotation` (0..360) | not 2 | |
| `textureFrameCount` (0..100), `textureFrameDuration` (0..10 s) | not 2 | sprite-sheet animation |
| `emissive` (hex) | not 2 | |
| `textureEncoding` (index into `config.encodingTypes`) | not 2 | |
| `aid` | not 2 | set from the call's `aid` argument (digits; `v` -> video) |
| `textureSmoothing` (bool) | 2 | linear vs nearest filtering |
| `spinSpeed` (-20..20), `spinAxis` | 2 | |
| `alphaTest` (0..1) | 2 | |
| `alwaysRender` (bool) | 2 | no frustum culling |
| `animSpeed`, `animStart` (clip name, ≤ 32 chars, safe charset) | 2 | |
| `castShadows`, `receiveShadows` | 2 | |
| `mimicSprite` (bool) | 1 | billboard |
| `opacity` (0..1), `forceTransparency` | all | |
| `side` (0 front, 1 back, 2 double), `doubleSided`, `backSided`, `frontSided` | all | |
| `thetaLength` (0.1..360, default 360), `sides` (3..64, default 12) | 3 | cones |

#### 3.3.2 Sign options (`API:1285-1303`)

`fontSize` (10..120, default 10), `fontColor` (hex, `#000`), `backgroundColor` (hex, `#fff`),
`backgroundInvisible` (bool), `textAlign` (`"left"`/`"center"`/`"right"` or 0..2), `outlineColor` (hex,
`#626262`), `outlineThickness` (1..50), `noScaleFont` (bool), `mimicSprite` (bool). The text is rendered
to a canvas texture of `(w*2, l*2)` pixels.

#### 3.3.3 AI spawn options (`API:1121-1201`, `API:3143-3168`)

`canMelee`, `canShoot` (bool); `modelScale` (0.01..100); `health` (int); `speed` (0..10); `turnSpeed`
(0..10); `gravity` (0..1); `roamTime` (1000..10000 ms); `roamRadius` (0..500); `chaseDis` (0..2000);
`hitBoxH`, `hitBoxW` (0.1..10, default 1); `fireRate` (200..10000); `shotSpread` (0..10); `shotBreak`
(0..5000); `shotArchY` (-90..90); `shotOffH` (bool); `shotDelay` (0..5000); `meleeDelay` (0..5000);
`meleeShake`, `shotShake` (0..1); `shotEffect`, `hitEffect` (0..9); `critDamage` (0..5); `meleeRate`
(200..10000); `meleeRange` (10..100); `meleeDmg` (1..200); `visionDis` (1..1000); `score` (0..10000);
`behaviorType` (0..20); `animStart`, `animSpeed`; `modelOffsetY` (-100..100); `modelRotation` (0..360);
`respawnT` (num), `respawnR`, `targAI`, `targPlr`, `friendly` (bool). Missing numbers default to 0 **then**
are clamped (so e.g. `fireRate` becomes 200, `meleeRange` 10, `meleeDmg` 1, `visionDis` 1, `roamTime` 1000).

#### 3.3.4 `addAsset` id letters (`API:2802-2840`)

The asset id string's digits are the asset; letters anywhere in it select options: `e` emissive white,
`n` normal map, `s` specular, `a` alpha test 0.02, `m` metalness 1, `r` roughness 1, `l` light map (uv2),
`p` displacement, `b` bump, `g` GLTF, `q` GLB, `f` FBX (animated loaders); no format letter = OBJ.
Example: `GAME.SCENE.addAsset("13075gn", 0, 0, 0, 1)` = GLTF model 13075 with a normal map.

---

## 4. Hooks (public actions the engine calls)

### 4.1 Hook table

A hook runs only if the script defines it as `public action <name>(...)`. Each call is one host entry
(loop budget reset, runtime errors contained, 1.14). Player ids are passed as strings (`player.id + ""`).

**Client script** (`SYN:3`; exported set from the compiled epilogue [OUT]):

| Hook | When it fires (v7 call site) | Arguments |
|---|---|---|
| `start()` | end of `Game.init`, i.e. after every `init` packet (map load and every round restart) and the script re-parse (`GJS:3789-3791`, `IDX:13505-13539`) | — |
| `update(num delta)` | every frame from `Game.update`, after players/AI/projectiles, before 3D render (`GJS:4960-4962`) | `delta` = frame ms (≤ 33) × `config.deltaMlt` |
| `render(num delta)` | every frame after the HUD overlay, with the overlay scaled by the UI scale (`IDX:17025-17031`) | `delta` = frame ms (no `deltaMlt`) |
| `onPlayerSpawn(str id)` | any player (self, others, bots) spawns (`IDX:14700-14702`) | spawned player's id |
| `onPlayerDeath(str id, str killerID)` | any player dies (kill feed, `IDX:15238-15240`) | victim id, killer id or `""` |
| `onPlayerUpdate(str id, num delta, obj inputs)` | each local input frame processed for prediction **and** each frame replayed during reconciliation (`PLJ:894-910`, `IDX:16842`, `IDX:14782-14791`) | own id, frame delta ms, inputs (4.2) |
| `onKeyPress(str key, num code)` | keydown that changes the key state, only while game controls are enabled (`IDX:16328-16330`, `CTL:125`) | `key` = DOM `KeyboardEvent.key` lowercased (4.3), `code` = legacy `which`/`keyCode` |
| `onKeyUp(str key, num code)` | keyup (`IDX:16566-16568`) | same |
| `onKeyHeld(str key, num code)` | keydown auto-repeat while already held (`CTL:159-160`) | same |
| `onControllerPress(str button, num code)` | gamepad button pressed (`CTL:1137-1141`) | button name (4.4), `code` = value of the mapped key bind or 0 |
| `onControllerUp(str button, num code)` | released | same |
| `onControllerHeld(str button, num code)` | held (repeats every gamepad poll) | same |
| `onMouseClick(num button, num x, num y)` | mousedown on the game view (`CTL:601-603`); if the mouse was temporarily unlocked it re-locks first | `button` = `which` (1 left, 2 middle, 3 right), window CSS px |
| `onMouseUp(num button, num x, num y)` | mouseup (`CTL:640-642`) | same |
| `onMouseScroll(num dir)` | wheel over the game view (`CTL:704-706`) | `-1` = wheel up, `1` = wheel down (`CTL:662-663`) |
| `onDIVClicked(str id)` | click on a script DIV/image or a map GUI element (`API:1700-1706`, `GOV:783-785`) | id of the clicked (innermost) element |
| `onNetworkMessage(str id, obj data)` | server script sent `"sd"` (`IDX:818-824`) | message id, data |

**Server script** (`SYN:4`):

| Hook | When it fires | Arguments |
|---|---|---|
| `start()` | end of every `Game.init` (every round; globals persist) (`GJS:3789-3791`) | — |
| `update(num delta)` | every server tick (`GJS:4960-4962`) | tick ms × `deltaMlt` (~33 ms at 30 Hz) |
| `onPlayerSpawn(str id)` | a player spawns (`VER:2287`; call site not in tree) [KN: right after the server spawns the player, before the spawn packet] | id |
| `onPlayerDeath(str id, str killerID)` | `players.kill` (`PLS:4889-4891`; v7 guards on `onPlayerSpawn` existing — KN guards on `onPlayerDeath`) | victim id; killer id whenever a killer object exists (including the victim itself), else `""` |
| `onPlayerDamage(str id, str doerID, num amount)` | damage > 0 that leaves the player alive (lethal hits fire only `onPlayerDeath`) (`PLS:4295-4299`) | victim, attacker id or `""`, damage |
| `onPlayerUpdate(str id, num delta, obj inputs)` | every input frame the server processes for every player (`PLJ:894-910`) | id, frame delta, inputs |
| `onCustomTrigger(str playerID, str customParam, num value)` | trigger action **Custom Action** executes (`TRG:1376-1390`) | triggering player id or `"NONE"`, the action's text value (`tct`) or `""`, the action's number value (`tc`, int) |
| `onNetworkMessage(str id, obj data, str playerID)` | client script `NETWORK.send` arrives (`SYN:4`; handler not in tree) | id, data, sender id |
| `onChatMessage(str msg, str playerID)` | a player chat message is accepted (`VER:1889`; not in tree) [KN: after the normal chat sanitising/rate limit, before broadcast; not for `/` commands; return value ignored] | text, sender id |
| `onPlayerLeave(str playerID)` | a player disconnects (`VER:2658`; not in tree) [KN: before the player is removed] | id |
| `onGameEnd()` | round ends, after `mode.onEnd` (`GJS:4665-4667`) | — |
| `onAdFinished(str playerID, bool success)` | rewarded video finished (`VER:1794`) — **never in KN** (no ads) | |
| `onServerClosed()` | game instance shuts down (`VER:1631`) [KN: `Game` teardown] | — |

Hooks listed for one side are never called on the other side, even if defined there.

### 4.2 The `inputs` object of `onPlayerUpdate` (`PLJ:864-910`)

| Field | Type | Meaning (input frame index) |
|---|---|---|
| `mouseY` | num | target pitch, radians (`input[2] / 1000`; 0 if the class locks pitch) |
| `mouseX` | num | target yaw, radians (`input[3] / 1000`) |
| `movDir` | num or `undefined` | movement direction radians (`config.movDirs[input[4]]` + class offset); `undefined` when not moving |
| `lMouse`, `rMouse`, `jump`, `reload`, `crouch`, `inter` | bool | `input[5]`, `[6]`, `[7]`, `[8]`, `[9]`, `[14]` |
| `scroll` | num | `-1`, `0`, `1` (`input[10]`) |
| `swap` | num | weapon swap request (`input[11]`) |
| `restK` | num | `input[13]` |

The object is a fresh copy: writing to it changes nothing. To suppress an input use
`player.disableDefault("<field>")`, applied right after the hook returns. `delta` =
`max(min, min(input[1], dltMx)) / deltaDiv` (ms).

### 4.3 Key names and codes (client)

`key` is the DOM `KeyboardEvent.key` value lowercased; `code` is the legacy `keyCode`. KN maps GLFW keys:

| GLFW | key | code | | GLFW | key | code |
|---|---|---|---|---|---|---|
| A..Z | `a`..`z` (shift does not change case after lowercasing) | 65..90 | | Space | `" "` | 32 |
| 0..9 | `0`..`9` (shifted: the symbol, e.g. `!`) | 48..57 | | Enter | `enter` | 13 |
| F1..F12 | `f1`..`f12` | 112..123 | | Escape | `escape` | 27 |
| Left/Up/Right/Down | `arrowleft` `arrowup` `arrowright` `arrowdown` | 37 38 39 40 | | Tab | `tab` | 9 |
| Shift | `shift` | 16 | | Backspace | `backspace` | 8 |
| Ctrl | `control` | 17 | | Alt | `alt` | 18 |
| `-` `=` | `-` `=` | 189 187 | | `,` `.` `/` | `,` `.` `/` | 188 190 191 |
| `;` `'` | `;` `'` | 186 222 | | `[` `]` `\` `` ` `` | `[` `]` `\` `` ` `` | 219 221 220 192 |
| Delete / Insert / Home / End / PgUp / PgDn | `delete` `insert` `home` `end` `pageup` `pagedown` | 46 45 36 35 33 34 | | CapsLock | `capslock` | 20 |
| Numpad 0..9 | `0`..`9` | 96..105 | | Meta | `meta` | 91 |

Text-producing keys follow the active keyboard layout (use GLFW's char callback for the `key` string,
fall back to this table). `INPUTS.keyDown(code)` uses the same codes.

### 4.4 Controller button names

`button_1` (A/Cross) `button_2` `button_3` `button_4` `shoulder_top_left` `shoulder_top_right`
`shoulder_bottom_left` `shoulder_bottom_right` `select` `start` `stick_button_left` `stick_button_right`
`d_pad_up` `d_pad_down` `d_pad_left` `d_pad_right` `vendor` (standard mapping indices 0..16), plus axis
names `stick_axis_left`, `stick_axis_right` (`REF/src/libs/gamepadInput.js:24-30`). KN: `client/src/gamepad.c`.

---

## 5. Integration points

### 5.1 Map JSON

* `scripts: {client?: str, server?: str}` (1.1). Absent on every v3.1.1 map: nothing runs; no behaviour
  changes for those maps.
* Map loading (`shared/src/map.c`) stores the decoded script texts on the `Map` (not parsed there).
  The server strips `server` from the map JSON it sends to clients (KN: from `NET_PACKET_MAP_DATA` payloads).
* The map catalogue (`shared/src/map_catalog.c`) should flag maps with scripts (v7 editor tags them
  "KrunkScript", `EDT:854`).

### 5.2 Triggers -> KrunkScript

* v7 trigger objects (`"i"` = trigger prefab) carry `"te"` (event index into `TRG` events, `MAP:586`) and
  `"tas"`: a list of actions `{ta, tc, tc2, tct, tcc, tce, tad, cd, cv0.., es/esa, ev, er, ef}`
  (`MMG:1942-2037`).
* **Custom Action = action index 58** (`TRG:1376-1390`; 0-based index in `module.exports.actions`).
  When it executes (server only, possibly delayed by `tad` seconds through `triggerQueue`, `GJS:4900-4907`)
  it calls `onCustomTrigger(player ? player.id : "NONE", tct || "", parseInt(tc) || 0)`.
* `tct` passes the trigger text filter: ≤ 70 chars and only `[\w:;()?,.|[\] \-+*$@=^!{}]` (`MMG:1992-2003`),
  otherwise it is dropped (the hook then receives `""`).
* Reference map: `TPL` "Triggers & Scripts": two triggers (`"te": 1`) with `{"ta": 58, "tct": "1"}` and
  `{"ta": 58, "tct": "2"}`; the server script logs `First Trigger` / `Second Trigger`.
* KN: the trigger action enum (`shared/include/triggers.h`, currently the v3.1.1 list) must be replaced by
  v7's 70 actions (other agent's work); this spec needs only `TRIGGER_ACTION_CUSTOM_ACTION = 58` to call
  `ks_server_on_custom_trigger(game, player_or_null, text, value)`.
* Related actions that touch the same data as scripts: **Save Data** (49) and **Load Data** (57) use the
  account storage of 2.6 (`TRG:1178-1204`, `TRG:1353-1375`); Load Data copies stored values into the
  player's trigger custom values (`custVals`, ≤ 30 keys, `CFG:1625`). Script player properties and trigger
  `custVals` are **separate** stores in v7.1.4 (no API reads `custVals`).
* `GAME.TRIGGERS.list/execute` are stubs (3.1.21): scripts cannot fire triggers.

### 5.3 Other engine wiring

| Feature | Wiring |
|---|---|
| Map GUI text elements | `#cusDIV<i>` click -> `onDIVClicked` (`GOV:761-795`) |
| Host option "KR rewards" | enables `PAYMENTS.giveKR` |
| Published map (`defaultSetts`) | required for `STORAGE` in production |
| `config.deltaMlt` (map/host slow-motion) | multiplies `update` deltas, not `render` or `fixedDelta` |
| Player input pipeline | `onPlayerUpdate` -> `disabledInputs` -> default behaviour unless `GAME.DEFAULT.disablePlayerBehaviour` (`PLJ:894-948`) |
| Player movement | `defaultMovement/Velocity/Rotation`, `disableShooting/Melee` flags on the player |
| Chat | `CHAT.*` lines (kind 2, colour) and `onChatMessage` |
| Kill/spawn events | client hooks from the kill/spawn packets; server hooks from `players.kill`/spawn |
| Round lifecycle | `start` per `init`, `onGameEnd` at round end, `TIME.freeze/unfreeze` on the round timer |
| Settings | `SETTINGS.set` changes and persists user settings (`client_settings`) |
| Editor | v7 script window, templates `SYN:3-4`, export as base64 (`EDT:3519-3526`); KN: the editor's script window (doc/v7/editor.md WP5, `client/src/editor/editor_script.c`) checks with `ks_validate` live and compiles the client script before Test |

---

## 6. Implementation plan for KrunkNative (C11, no JS)

### 6.1 Architecture

```
                    shared/ (no GL, no GLFW, no FreeType, no Lexbor)
 ┌──────────────────────────────────────────────────────────────────────────┐
 │ ks_lex -> ks_parse -> AST -> ks_check (types, scopes) -> ks_compile -> KsProgram (bytecode) │
 │ ks_vm (interpreter, values, GC, limits, runtime errors)                                   │
 │ ks_api (GAME.* binding table: path, side, TYP type, C fn) + ks_stdlib (UTILS/math/TIME)   │
 │ ks_hostobj (player/AI/map object/path node wrappers, shared by both sides)                │
 │ ks_script (instance lifecycle, Game hook dispatch points, rate limiters)                  │
 └──────────────────────────────────────────────────────────────────────────┘
        ▲ KsHost vtable (side = client)                ▲ KsHost vtable (side = server)
 client/src/ks/: UI (html_dom), OVERLAY (canvas2d),     server/src/ks_server_host.c (server core):
 SCENE (Scene/Mesh), SOUND (audio.c), CAMERA,           NETWORK, CHAT, STORAGE (kn_brain or offline
 INPUTS, COOKIES, SETTINGS, PLATFORM, MODS              file store), ADMIN, PAYMENTS, CONFIG, AI.spawn,
                                                        TIME.freeze, inbound "sd" handling
```

* The **VM and every game-logic hook point are in shared/** (`Game`, `player_process_input`, triggers),
  because both the client game (prediction: client `onPlayerUpdate`) and the server game call hooks.
  `shared/` sees only the `KsHost` vtable.
* The **server host lives in `server/src/`**, in the server core that `server_bin` runs and that the
  offline sandbox links into the client (`server/include/server_sandbox.h`, `client/src/net/offline.c`),
  so offline play runs the real server script code with no extra path. It must not gain GL/GLFW deps.
* The client host lives in `client/src/ks/`. Shared code never includes client or server headers; the
  host vtable is the boundary.
* One `KsScript` per (Game, side); the compiled `KsProgram` can be shared read-only.

### 6.2 Public C API (`shared/include/ks.h`) — the contract between work packages

```c
typedef struct KsProgram KsProgram;   // immutable compiled script
typedef struct KsScript  KsScript;    // one running instance (globals + heap)
typedef enum { KS_SIDE_CLIENT, KS_SIDE_SERVER } KsSide;

typedef struct {
    int line, col;
    char message[256];
} KsDiag;

// Front end (WP1) + compiler (WP2)
KsProgram *ks_compile(const char *source, size_t len, KsSide side, KsDiag *err, KsDiag *warnings, int max_warnings, int *warning_count);
void       ks_program_free(KsProgram *);

// Instances (WP2)
typedef struct KsHost KsHost;         // 6.5
KsScript  *ks_script_new(const KsProgram *, const KsHost *, void *host_user); // runs global initialisers
void       ks_script_free(KsScript *);
int        ks_script_has_hook(const KsScript *, const char *name);
// Hook call with already-converted arguments; returns 0 ok, -1 runtime error (already logged).
int        ks_script_call_hook(KsScript *, const char *name, const KsValue *args, int argc);
int        ks_script_failed(const KsScript *);                    // load error -> never call hooks

// Values (WP2) — opaque handles owned by a KsScript heap
typedef struct KsValue { uint8_t tag; union { double num; int b; struct KsObj *ref; } as; } KsValue;
enum { KS_UNDEF, KS_NULL, KS_BOOL, KS_NUM, KS_STR, KS_ARR, KS_OBJ, KS_FN, KS_HOST };
KsValue ks_num(double), ks_bool(int), ks_undef(void), ks_null(void);
KsValue ks_str(KsScript *, const char *utf8, size_t len);
KsValue ks_obj_new(KsScript *);  KsValue ks_arr_new(KsScript *);
void    ks_obj_set(KsScript *, KsValue obj, const char *key, KsValue v);
KsValue ks_obj_get(KsScript *, KsValue obj, const char *key);
void    ks_arr_push(KsScript *, KsValue arr, KsValue v);
KsValue ks_host_object(KsScript *, const KsHostClass *, void *native, uint32_t native_id);
size_t  ks_to_string(KsScript *, KsValue, char *buf, size_t n);   // ToString (1.12)
double  ks_to_number(KsValue);                                    // ToNumber
// (de)serialisation for networking/storage (2.5)
size_t  ks_serialize(KsScript *, KsValue, uint8_t *out, size_t cap);
KsValue ks_deserialize(KsScript *, const uint8_t *in, size_t len, int *ok);
size_t  ks_json_length_prefixed(KsScript *, KsValue);             // 2000-char rule
```

### 6.3 AST (`shared/include/ks_ast.h`, produced by WP1, consumed by WP2)

Nodes carry `line`, `col`, and after checking a resolved `KsType` (`{base: NUM|STR|BOOL|OBJ|ANY|VOID|ACTION, dims}`).

* Top: `Program{globals[], actions[]}`, `ActionDecl{name, is_public, ret_type, params[], body}`, `Param{name, type, modifier}`.
* Statements: `VarDecl{name, type, init?}`, `If{cond, then, else?}`, `While{cond, body}`,
  `For{init?, cond?, step?, body}`, `ForEach{var_name, var_type, iterable, body}`, `Return{expr?}`, `Break`,
  `Continue`, `AddTo{target, value}`, `Remove{target}` (index or member), `Assign{target, op, value, target_cast?}`,
  `IncDec{target, op, prefix, target_cast?}`, `ExprStmt{call}`, `Block{stmts[]}`.
* Expressions: `Num`, `Str`, `Bool`, `Ident{slot}`, `Member{obj, name}`, `Index{obj, index}`,
  `Call{callee, args[], kind: ACTION|ACTION_VALUE|GAME_API|METHOD}`, `GameRef{api_id}` (resolved `GAME.x.y`
  path), `Unary{op, expr}`, `Binary{op, l, r}`, `Logical{op, l, r}`, `Ternary`, `Cast{type, expr}`,
  `KeywordOp{LENGTHOF|NOTEMPTY|TONUM|TOSTR, expr}`, `HasProp{obj, name}`, `ArrayLit{elem_type, items[]}`,
  `ObjectLit{keys[], values[]}`, `ActionRef{action_index}`.

### 6.4 Runtime design notes

* **Execution**: compile the checked AST to a compact stack bytecode (`ks_compile.c`) and interpret it
  (`ks_vm.c`, computed-goto optional). Each loop back-edge does the `LC` check with the loop's line/col;
  every instruction decrements the instruction budget. Calls push frames (depth ≤ 2000).
  A tree-walking interpreter is acceptable as a first cut if it keeps the same `ks.h` API.
* **Values**: NaN-free tagged struct (6.2). Strings: immutable, refcounted or GC'd, UTF-8 bytes + cached
  UTF-16 length + `is_ascii` flag; UTF-16 index helpers for `lengthOf`/index/`truncateTxt`.
  Interning for property keys (objects use interned key ids).
* **Objects**: insertion-ordered open-addressing hash map (key id -> slot) + dense entry array with
  tombstones; `remove` keeps order of the rest. **Host objects**: `KsHostClass {name, props[] {name,
  getter, setter, is_method, method_fn, is_subobject}}` + a per-instance expando map for script-added
  properties; the expando map is stored on the engine side keyed by `(class, native_id)` so it survives
  re-fetches (v7 `_protected` cache) and is dropped when the engine object dies (player leaves / AI removed).
* **Arrays**: dense `KsValue` vector.
* **GC**: precise mark-and-sweep per `KsScript` heap; roots = globals, VM stack, pending callbacks (storage,
  asset onLoad), host-held handles (`ks_handle_pin/unpin`). Collect only between host entries or when the
  heap grows 2x. Cycles (`o.self = o`) are therefore safe. No finalizers.
* **Number formatting / parsing**: `ks_number.c` (1.12). Unit-test against a table generated from Node
  (`tools/ks/gen_number_table.js`, run once by the developer, table checked in).
* **Errors**: one `KsError{line, col, message}` path for compile and runtime; runtime errors are formatted as
  `RUNTIME ERROR: ...` (1.14) and sent to the host `log` callback with `is_error = 1`.
* **No closures needed**: actions are top-level; action values are `{program, action_index}` or
  `{host_fn}`. API callbacks (`STORAGE.load(..., cb)`) store the action value plus the `KsScript` and are
  invoked later as a new host entry. If the script was freed, the callback is dropped.
* **Determinism**: `UTILS.rand*` from a per-instance PCG32 seeded from the game seed (server) or a random
  seed (client); `TIME.now()` = wall-clock ms (`double`).

### 6.5 Host interface (`KsHost`)

```c
typedef struct KsHost {
    KsSide side;
    void (*log)(void *user, int is_error, const char *line);            // console / NET_PACKET_SCRIPT_LOG
    double (*now_ms)(void *user);
    const KsApiImpl *api;          // array indexed by KsApiId: C function per GAME.* entry for this side, NULL = gated no-op
} KsHost;
typedef KsValue (*KsApiFn)(KsScript *, void *user, const KsValue *args, int argc);
```

`shared/src/ks/ks_api_table.c` lists every `GAME.*` member once: `{path "SCENE.addCube", id KS_API_SCENE_ADDCUBE,
kind FN|PROP|NAMESPACE, sides CLIENT|SERVER|BOTH, ret_type (from TYP), rate_key/limit}`. The checker uses it for
rule 9/10; the VM dispatches through `host->api[id]` (missing = gated no-op returning `undefined`).
Properties like `GAME.CAMERA.position.x` and `GAME.CAMERA.envZone` are `PROP` entries with getter/setter.
`UTILS.*`, `math.*`, `TIME.*` are implemented once in shared and installed on both sides.

### 6.6 Module and file list

| File | WP | Content |
|---|---|---|
| `shared/include/ks.h` | WP1/2 | public API (6.2) — written first, frozen |
| `shared/include/ks_ast.h` | WP1 | AST (6.3) |
| `shared/src/ks/ks_lex.c` | WP1 | lexer (1.2), fullwidth-paren mapping, base64 detection/decoding helper |
| `shared/src/ks/ks_parse.c` | WP1 | recursive-descent parser (1.3), error recovery off (first error) |
| `shared/src/ks/ks_check.c` | WP1 | scopes, types, hook signature checks, API path resolution, diagnostics catalogue (1.15) |
| `shared/src/ks/ks_api_table.c` | WP1 | `GAME.*` table (3.x) with `TYP` return types and sides |
| `shared/src/ks/ks_compile.c` | WP2 | AST -> bytecode |
| `shared/src/ks/ks_vm.c` | WP2 | interpreter, frames, limits, runtime errors |
| `shared/src/ks/ks_value.c` | WP2 | strings, objects, arrays, host objects, GC |
| `shared/src/ks/ks_number.c` | WP2 | Number::toString, parseFloat, parseInt, StringToNumber, ToInt32 |
| `shared/src/ks/ks_serial.c` | WP2 | binary encoding, JSON length, `GAME.log` JSON pretty printer (`JSON.stringify(x,null,2)` layout) |
| `shared/src/ks/ks_stdlib.c` | WP2 | `UTILS`, `math`, `TIME` (3.1.5, 3.1.7) |
| `shared/src/ks/ks_script.c` | WP3 | `KsScript` lifecycle glue for `Game`, hook dispatch helpers, rate limiters (3.0) |
| `shared/src/ks/ks_hostobj.c` | WP3 | player / AI / map object / path node classes (3.2) |
| `server/src/ks_server_host.c` | WP3 | server `KsHost` (server core, also linked into the offline sandbox): NETWORK (+ inbound `NET_PACKET_SCRIPT_MSG`), CHAT, STORAGE (callback), ADMIN, PAYMENTS, CONFIG, AI.spawn/remove/list, TIME.freeze, server hooks from connection events (leave, chat) |
| `shared/include/ks_game.h` | WP3 | `Game` hooks API: `ks_game_load_scripts`, `ks_game_on_*` (called from game.c, player.c, triggers.c, net) |
| `client/src/ks/ks_client_host.c` | WP4 | client `KsHost`, PLAYERS/OBJECTS reuse, hook calls from input/events |
| `client/src/ks/ks_ui.c` | WP4 | `GAME.UI` on `html_dom.h` (+ `onDIVClicked` listeners), protected ids, cssCheck |
| `client/src/ks/ks_scene.c` | WP4 | `GAME.SCENE`, obj3D/bone classes, script-owned meshes/lights/signs |
| `client/src/ks/ks_sound.c` | WP4 | `GAME.SOUND`, sound class |
| `client/src/ks/ks_input.c` | WP4 | `GAME.INPUTS`, `GAME.CAMERA`, key-name/code table (4.3), controller names (4.4) |
| `client/src/ks/ks_misc.c` | WP4 | COOKIES, SETTINGS (`ks_settings_map`), PLATFORM, MODS, ADS, setTitle, changeGame |
| `client/src/ks/canvas2d.c`, `client/include/canvas2d.h` | WP5 | Canvas2D subset: state stack, affine transforms, path (lines, arcs, ellipses, quadratic/cubic béziers flattened), nonzero fill (stencil-then-cover or ear-clipping of flattened polygons), stroke (joins: miter/bevel/round), text via the HUD font, images via `load_texture` |
| `client/src/ks/ks_overlay.c` | WP5 | `GAME.OVERLAY` on canvas2d |
| `server/src/ks_storage.c` | WP6 | kn_brain storage backend; offline file store when the server core runs as the sandbox (no brain) |
| `tools/ks/ksc.c` | WP1/2 | CLI: `ksc check file.krnk`, `ksc run file.krnk --hooks start,update:16 --log` with a mock host, `ksc import-js file.js\|map.json` (WP7) |
| `shared/src/ks/ks_import_js.c`, `shared/include/ks_import_js.h` | WP7 | compiled-JS importer: prelude match, JS-subset lexer/parser undoing the 1.16 helper patterns, KrunkScript emitter (keeps the original line numbers from `BC`/`RE`/`FC`) |
| `shared/tests/ks/*.krnk` + `*.out` | all | conformance suite (6.7) |
| `shared/tests/ks_conformance_test.c` | WP2 | runs the suite (CMake target `ks_test`, part of `make test`) |

Build: add the `shared/src/ks/*.c` files to `SHARED_SOURCES` (they have no new dependencies; cJSON is
already available for map parsing), `client/src/ks/*.c` to the client, and the new test targets to
`CMakeLists.txt` next to `proc_input_test`. A `make ks-test` target runs `ks_test` and `ksc check` over the
corpus.

### 6.7 Test plan

1. **Corpus compile test** (WP1 gate): `ksc check` must accept every corpus script: the 15 template
   scripts and the 3 distinct map scripts (`kanji`, `citadel`, `Krunk_Plaza`), checked in as
   `shared/tests/ks/corpus/*.krnk` (decoded with the commands in 0.3; they are Krunker content,
   keep provenance comments at the top). Expected: 0 errors, no warnings except implicit casts.
2. **Negative tests**: one `.krnk` per catalogue entry of 1.15 with the expected `line:col message` in
   `*.err`.
3. **Conformance (runtime)** — `*.krnk` + `*.out` (expected `GAME.log` lines, one per line, errors prefixed
   `!`), run by `ks_conformance_test` with the mock host calling `start()` once (plus listed hooks). Cover:
   * number formatting (`toStr` of 0.1+0.2, 1/3, 1e21, 1e-7, -0, NaN, 2**53, 0xFF), parseFloat casts
     (`(num) "12abc"`, `(num) true`, `(num) "0x10"`), `toNum`;
   * string ops: concat, `lengthOf` of non-ASCII (`"é"` = 1, `"😀"` = 2), `truncateTxt`, `replaceText`
     (first match), comparisons;
   * equality strictness, truthiness table, `&&`/`||` returning operands, ternary;
   * arrays: literals incl. nested, `addTo`, `remove a[i]`, `lengthOf`, out-of-range read/write errors with
     line:col, `a[1.7]`, in-place cast mutation (`(num[]) a` turns `"1"` elements into `1`);
   * objects: literal, nested write, add/remove keys, `hasProp`, `notEmpty`, iteration order after
     remove/re-add, aliasing;
   * control flow: nested loops with break/continue, loop budget exactly 1,000,001 iterations OK /
     1,000,002 -> `Loop timed out.`, recursion depth limit;
   * runtime error containment: an error in `update` leaves globals as written and the next call runs;
   * `GAME.log` formatting of objects (`JSON.stringify(x,null,2)` with un-prefixed keys) and arrays;
   * `UTILS`/`math` functions against values generated from Node (`tools/ks/gen_stdlib_table.js`).
   Generate expected outputs once with a tiny Node reference harness that implements the 1.16 lowering by
   hand for each test (checked in as `tools/ks/ref/*.js`), so expectations come from real JS semantics.
4. **Host tests** (WP3/WP4): scripted offline games with `KRUNKNATIVE_HEADLESS=1`, `KRUNKNATIVE_INPUT_SCRIPT`
   and `KRUNKNATIVE_FIXED_DELTA` (AGENTS.md): the `Triggers & Scripts` template map (walk into a trigger,
   expect `[KRUNKSCRIPT] First Trigger` in the client log via `NET_PACKET_SCRIPT_LOG`), `Basic Networking`
   (press G, expect the DIV text `You Passed Age Validation` or `Failed`), `Bouncing Ball`, `Popup Window`,
   `Converting 3D to 2D` and `Simple .io Game` screenshots (`make smoke` style, compare to reference
   captures taken from v7 in a browser if available, else visual review).
5. **Map tests**: `kanji` (v7 version with its client script: interaction window DIVs exist at start,
   `update` typewriter effect), `Krunk_Plaza` (keypad DIV grid of 12 keys).
6. **Fuzzing**: `ksc check` on mutated corpus files (libFuzzer or a simple mutation loop in CI), asserting
   no crash/leak (ASan); VM fuzz with random hook calls under limits.
7. **Rate-limit tests** (unit, mock clock): `CHAT.send` 2/s, `NETWORK.broadcast` 9/s, `send` 19/s/target,
   storage 1 per 10 s, in-flight locks.

### 6.8 Work packages (parallelisable)

Freeze `ks.h` (6.2), `ks_ast.h` (6.3), `KsHost`/`KsApiImpl` (6.5) and the API id enum first (½ day, WP1 owner),
then:

| WP | Scope | Depends on | Deliverables / acceptance |
|---|---|---|---|
| **WP1 Front end** | lexer, parser, checker, `ks_api_table.c`, diagnostics, `ksc check` | frozen headers | corpus compile test passes; negative tests; no allocations leak (ASan) |
| **WP2 Runtime core** | compiler to bytecode, VM, values/GC, number formatting, serialisation, stdlib (UTILS/math/TIME), `ksc run`, conformance runner | ks_ast.h; can start with hand-built ASTs | all conformance tests; loop budget exact; perf: Game of Life template `render` at 60 fps uses < 2 ms/frame on llvmpipe-class CPU |
| **WP3 Shared/server host + game integration** | `KsScript` in `Game` (load from map JSON, side selection, lifecycle 2.2), server hooks (4.1), player/AI/map wrappers, NETWORK/CHAT/ADMIN/PAYMENTS/CONFIG/TIME.freeze/DEFAULT, custom trigger action 58, `NET_PACKET_SCRIPT_MSG/LOG/SYNC`, custom sync values, rate limits; the offline sandbox server (`server/src/sandbox.c`) runs the server script through the same code | ks.h (can use a stub VM that only logs hook calls until WP2 lands) | Triggers & Scripts and Basic Networking host tests; `proc_input_test` unchanged; server binary has no new deps |
| **WP4 Client host** | client hooks (keys, mouse, controller, DIV clicks, spawn/death, update/render ordering), UI (html_dom), SCENE/obj3D/bones/lights/signs, SOUND, CAMERA, INPUTS, COOKIES, SETTINGS, PLATFORM, MODS stubs | ks.h, WP3 for networking | Popup Window, Bouncing Ball, Flashlight, Attaching templates behave; web build compiles (no pthreads; GLES3-only calls) |
| **WP5 Overlay canvas** | `canvas2d` module + `GAME.OVERLAY` | none (standalone, test with its own scenario) | golden screenshots for rects, rotated rects, circles/ellipses, lines, arcs, béziers, text alignments, images; Simple .io Game and Snake Demo render |
| **WP6 Persistence & services** | STORAGE (kn_brain + offline file store), Save/Load Data trigger interplay, COOKIES persistence check, giveKR/kick/ban backends, optional editor script window (edit `scripts.client/server`, base64 export) | WP3 | storage unit tests (validation messages, 10 s limit, locks), offline round-trip |
| WP7 (optional, landed) | importer for compiled-JS client scripts (the 1.16 lowering is regular enough to translate back to KrunkScript AST: recognise the prelude, `function V_x(..){try{LC=0;...}catch...}`, `this.GAME.V_A.V_b.bind(this)(...)`, `BC(...)`, `A(x,F)`, `S()`, `L()`, `let V_x`) | WP1 | `SniperRoyale2` and `Verb_Arena` scripts run |

WP1, WP2 (with hand-built ASTs), WP3 (with a stub VM) and WP5 can start in parallel on day 1. WP4 starts
once `ks.h` is frozen; it needs WP2 for end-to-end runs.

### 6.9 Hard parts without JavaScript

| Topic | Why it is hard | Approach |
|---|---|---|
| Number -> string | JS shortest round-trip formatting with ES layout rules is visible in every `toStr`/log | `ks_number.c` with a shortest-digits loop or Ryu; table-driven tests from Node |
| String semantics | JS strings are UTF-16: `lengthOf`, indices, `substring` in `truncateTxt`, comparisons by code unit, surrogate pairs | UTF-8 storage + UTF-16 index mapping helpers; ASCII fast path |
| parseFloat/parseInt/ToNumber | three different JS conversions, all observable | implement each exactly (1.12) |
| Dynamic objects | v7 objects are JS objects with prefixed keys, Proxies for API objects, getters/setters, non-writable sub-objects, `_protected` caches | ordered hash map + host classes with property descriptors + engine-side expando maps (6.4) |
| Closures | none in the language: action values only reference top-level actions; callbacks are `{script, action}` pairs re-entered as new host entries | no closure support needed |
| Garbage | JS GC handled cycles; refcounting would leak `o.self = o` | per-script mark-sweep heap |
| JSON | `GAME.log` uses `JSON.stringify(x, null, 2)` (key order, number format, string escaping, `undefined`/function omission, `null` in arrays) and the 2000-char limit counts prefixed JSON | `ks_serial.c` JSON writer with JS escaping (`\"`, `\\`, `\n`, `\u00XX` for controls, lone surrogates as `\uDXXX`) |
| String formatting | there is no `printf` in KS; all formatting is `+` and `toStr` | nothing beyond ToString |
| Canvas 2D | v7 used the browser canvas | `canvas2d.c` subset (WP5) |
| DOM/CSS | v7 used the browser DOM | KN's in-house HTML engine already implements the DOM ops (`html_dom.h`); `style[prop]` via `html_dom_set_style` |
| Async callbacks | storage/asset callbacks arrive later | queue on the `Game`, invoke from the main thread as new host entries |
| Compiled-JS maps | 2 maps ship JS | WP7 importer: JS -> KrunkScript source (types from literals, hook signatures and the `GAME.*` table) |

---

## 7. Open questions and KN decisions register

| # | Question | Evidence | KN decision |
|---|---|---|---|
| 1 | Exact compile-error texts | compiler absent | catalogue 1.15 |
| 2 | `for`-each syntax | `VER:1253`, `OE` helper | `for (obj e : o)` with `e.key`, `e.value` |
| 3 | `action` parameter type syntax | `VER:1286` | `action`, `T action`, `action(T,..)` all accepted |
| 4 | `static`/`dynamic` meaning | templates | accepted, ignored |
| 5 | `toNum` = `parseFloat` or `parseInt`? | `N` exists in the prelude but no corpus use | `parseFloat` (same as `(num)`) |
| 6 | Do `(str)`/`(bool)`/`(obj)` apply element-wise to arrays? | `(num)` -> `A(x,F)`; `(str)` -> `S(x)` | element-wise for all casts |
| 7 | Bounds check on index **writes** | `BC` seen on reads | checked; writes never grow arrays |
| 8 | Does every action reset `LC`, or only hooks? | only public actions seen | reset per host entry |
| 9 | Server `onNetworkMessage`, `onChatMessage`, `onPlayerLeave`, `onPlayerSpawn`, `onServerClosed` call sites | not in tree | 4.1 |
| 10 | Server inbound `sd` limits | changelog only | 20/s/player, 2000 chars |
| 11 | `NETWORK.send` return on client | returns `undefined` in v7 | `true`/`false` |
| 12 | Global initialiser error | v7 fails the map | KN disables the script, map loads |
| 13 | Implicit cast of `any` into typed slots | corpus always casts | allowed with warning |
| 14 | Storage op type 5 | `VER:1680` | delete key |
| 15 | Custom sync server reader | `serverHooks.checkCustSync` absent | read `player.<name>` primitives |
| 16 | AI `rotation` bug | `API:582-595` | fixed to AI yaw/pitch |
| 17 | `RAYCAST.from` | broken in v7.1.4, stub in live | `undefined` |
| 18 | Point/spot/dir/rect lights in the KN renderer | renderer is directional+ambient | implement point/spot when the renderer supports them; until then create invisible placeholder objects (methods work, no light) and warn once |
| 19 | Editor test play runs server scripts? | v7: no | no (parity) |
| 20 | Compiled-JS map scripts | 2 corpus maps | WP7 importer translates the 1.16 lowering back to KrunkScript; other JS is rejected with a diagnostic. Not observed in the corpus and translated from the prelude's helper definitions: `DE`, `delete`, `HP`, `NE`, `FC`, `for (let V_e of OE(x))`, `.push` |

---

## Appendix A. Corpus feature checklist (the front end must accept all of these)

| Feature | Example (file) |
|---|---|
| `#` comments, trailing comments | everywhere |
| globals with API initialisers | `obj bounds = GAME.OVERLAY.getSize();` (Snake Demo) |
| typed actions | `obj action genPebble() { return {...}; }` (Simple .io Game) |
| `public action` hooks with fewer params | `onCustomTrigger(str playerID, str customParam)` (Triggers & Scripts) |
| `static obj` / `dynamic obj` params | `onPlayerUpdate(str id, num delta, static obj inputs)`, `onNetworkMessage(str id, dynamic obj data, str playerID)` |
| object literals with trailing comma | `obj css = { ..., instructions: "...", };` (kanji) |
| nested array literals with spaces | `num[] [num [(width - 40) / 2, (height - 40) / 2]]` (Snake Demo) |
| empty typed arrays | `obj[] pebbles = obj[];`, `num[][] tiles = num[][];` |
| 2D string array literal | `str[][] rows = str[][str['q','w'], str['a']];` (Krunk_Plaza) |
| casts on reads | `(num) ball.speed * delta`, `(str)data.text`, `(bool) coords.onScreen`, `(obj) dat.getBone("Palm2L")`, `(num[][]) player.pos` |
| casts on assignment targets | `(num) ball.x += ...;`, `(num) player.tail++;`, `(num) window.height -= (num) ui.height;` |
| `lengthOf`, `addTo`, `remove`, `notEmpty`, `toStr` | `lengthOf (num[][]) player.pos`, `addTo buffer {x: i, y: j, val: 1};`, `remove player.pos[0];`, `notEmpty plr`, `toStr grid.scale + "px"` |
| `!!`, ternary, `%`, unary minus on casts | `!!tPlr`, `valid ? "Passed" : "Failed"`, `-((num) ball.x) % gridSize` |
| JS-reserved identifiers | `num[] new = ...` (Snake Demo), `str function` (Game of Life) |
| trailing comma in call args | `GAME.UI.addDIV("title", true, );` (Game of Life) |
| `};` after blocks | Game of Life |
| `else if` chains, `while`, nested `for` | everywhere |
| array element assignment, 2D index | `new[0] = (new[0] + width) % width;`, `tiles[i][j] = track;` |
| API object methods and chained properties | `torch.lookAt(...)`, `lamp.light.position.x = ...`, `leftCube.attachTo(leftPalm, 0, 0, 0);` |
| `GAME.UTILS.*`, `UTILS.*`, `Math.*` | `GAME.UTILS.hexFromHue(...)`, `UTILS.truncateTxt(...)`, `Math.PI2` |
| extra args to API calls | `GAME.changeGame((str)data.map, { host: true, private: true });` |
