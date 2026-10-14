# Krunker Strike — v1 design decision

Status: scaffold landed (this doc + the flavor seam); gameplay/UI/assets fan out per module.
Owner of this doc: coordinator. Date: 2026-10-14.

## What Krunker Strike is

Krunker Strike is a **product flavor** of Krunker Box Office: the same C11 engine, the same
`shared/` game core, the same renderer and the same Krunker v10.0.2 ruleset
(`shared/include/ruleset.h`), shipped under a distinct brand with a **small, bounded set of
gameplay deltas** layered on top. It is not a fork and not a second engine: one codebase, one
build system, one protocol. A player running "Krunker Strike" is running `client_bin`/`server_bin`
with the Strike *flavor* selected.

The goal of v1 is the smallest thing that is still a recognisably separate product: a name, an
identity the server advertises and the client shows, and the **seam** through which a handful of
gameplay tweaks will differentiate Strike from the base game — with exactly one tweak wired as the
seed so the mechanism is real, not theoretical.

## Flavor vs ruleset (why a new axis)

`Ruleset` answers "which Krunker *version* are we playing" (v10.0.2 is the only one). A flavor is
orthogonal: "which Krunker Box Office *product* is this". Both are **one-per-process**, chosen at
startup; a Strike server and a base server never mix players (enforcement is a fan-out item, see
flavor-net). Keeping them separate means Strike never has to touch ruleset tables, and a future
second flavor costs one `GameFlavor` enumerator plus a `FlavorConfig` row.

```
GameFlavor   = FLAVOR_BASE (0) | FLAVOR_STRIKE (1)        shared/include/flavor.h
FlavorConfig = { flavor, key, display_name, <gameplay knobs...> }
```

`flavor.h`/`flavor.c` are **pure C in the shared core** (no GL/GLFW/net), so server, client and the
tests all link them, exactly like `KN_PRODUCT_NAME`.

## The smallest meaningful difference (v1)

1. **Brand / display name.** `flavor_display_name()` returns `"Krunker Box Office"` for base and
   `"Krunker Strike"` for strike. This is the only *player-facing* name change: window title,
   `--version`, the startup banner, and (fan-out) the menu wordmark. **Identifiers do not change** —
   the repo, `KRUNKNATIVE_*` variables, `kn_*` targets, user agents and protocol strings stay
   `KrunkNative`/`krunknative`, same rule as `build_info.h`.
2. **Server advertises it.** The server-info query gains an optional `flavor` field
   (`shared/src/server_info.c`); it is **absent for base**, so base servers answer byte-for-byte as
   before, and `"strike"` for a Strike server. The client learns a server's flavor from this (and,
   later, from INIT — flavor-net).
3. **One gameplay knob, wired as a seam.** `FlavorConfig.damage_mult` (global outgoing-damage
   multiplier) is defined now and `1.0` (neutral) for *both* flavors in v1, so the default build is
   the base game unchanged. The **flavor-gameplay** module owns (a) choosing Strike's actual v1
   deltas and their values and (b) the `shared/` code that reads `flavor_current()` in the damage /
   movement path. Proposed v1 Strike identity for that module to confirm: *hardcore* — higher damage
   (`damage_mult ≈ 1.25`) and no passive health regen, so fights are faster and positioning matters
   more. Keep it to 1–3 knobs for v1.

Selection:
- Server: `server_bin --flavor strike`, or `server.cfg`/config `gameserver.flavor strike`
  (`--flavor` wins; an unknown value is rejected on the CLI, ignored with an error line in config).
- Client: `client_bin --flavor strike`, `KN_FLAVOR=strike`, or a Strike-defaulted build
  (`-DKRUNKNATIVE_FLAVOR=strike` / `make strike`). The runtime flag always overrides the build
  default.

## What landed in the scaffold (this patch)

- `shared/include/flavor.h`, `shared/src/flavor.c` — the `GameFlavor`/`FlavorConfig` model, the
  process flavor (`flavor_current/get/set/set_key`), the key/name/config lookups, and the
  compile-time default (`KN_FLAVOR_DEFAULT`).
- `shared/include/server_info.h`, `shared/src/server_info.c` — the optional `flavor` wire field
  (absent = base).
- `server/src/main.c`, `server/src/config.c`, `server/include/server_config.h`,
  `server/src/info_query.c` — `--flavor`, `gameserver.flavor`, the startup `Flavor:` log, the
  `--version` line, and the server-info field.
- `client/src/main.c` — `--flavor`/`KN_FLAVOR`, branded window title, banner and `--version`.
- `CMakeLists.txt`, `CMakePresets.json`, `Makefile` — the `KRUNKNATIVE_FLAVOR` option, a `strike`
  preset, and `make strike` / `run-strike` / `server-strike`. Base/web/official builds are
  unchanged (base defines nothing extra).
- `server/tests/strike_flavor_e2e.py` — base vs strike servers, a direct-connect spawn on each,
  server-info field, config paths, and the `--version` branding.

Everything Strike-specific is **gated on `flavor_get() != FLAVOR_BASE`** or on the build default, so
a default build and the web/official packages behave exactly as before.

## Module breakdown (fan out one owner each)

See `~/.cache/kn-strike/STRIKE-BRIEF.md` for the per-module interfaces, test bar and rules.

| Module | Owns | One-line scope |
|---|---|---|
| flavor-core *(landed)* | `shared/{include/flavor.h,src/flavor.c}` | the flavor model + process flavor (the contract for everyone else) |
| flavor-gameplay | `shared/src/` damage/health/movement readers of `FlavorConfig` | pick + wire Strike's v1 gameplay deltas (hardcore: damage, regen) |
| flavor-net | `shared/` INIT + `client/src/net/` | carry flavor in INIT, refuse cross-flavor peers (like the ruleset mask) |
| flavor-ui | `client/src/ui/`, `ui/` | menu wordmark / brand, the version picker's absence, Strike theming hooks |
| flavor-assets | `assets/`, `ui/img/` | Strike logo, window icon, loading art (reference data; code-side hooks) |
| flavor-matchmaking | `server/src/` finder, `services/` coordinator | list Strike lobbies separately; a Strike region/pool in the brain |
| flavor-packaging | `tools/release/`, `launcher/` | a `make strike` dedicated-server dist + a Strike client package |
| flavor-docs/e2e | `doc/v10/`, `server/tests/`, `tools/uidiff/` | per-module e2e + a uidiff scenario for the Strike brand |
