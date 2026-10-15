#pragma once
// The gameplay a product flavor (shared/include/flavor.h) changes, centralised so every consumer
// asks the same way. A flavor with rules (Krunker Strike: FRVR's data, shared/include/strike_rules.h)
// plays its own weapon / class / mode tables through ruleset_current(); these are the gates and
// defaults around them. Every helper is the base game's own answer when the flavor has no rules
// (FLAVOR_BASE), so a base build runs unchanged byte-for-byte. Gate flavor behaviour here, never by
// indexing a flavor table.
//
// Pure C; part of the shared core linked by server_bin, client_bin and the tests.
#include <stddef.h>
#include <shared.h> // GameConfig
#include <flavor.h>

// The class a player who picked `class_index` plays (before any mode-forced class): the pick itself,
// or under Strike the starter class (Triggerman) when Strike has no such class.
int flavor_class_pick(int class_index);
// 1 when a player or a bot may pick `class_index` (base: always; Strike: its six classes).
int flavor_class_allowed(int class_index);
// 1 when mode `mode_id` exists in the flavor (base: always; Strike: its 35-mode table, no Trade /
// Impulsed). Hosting and the server's mode lists check it.
int flavor_mode_allowed(int mode_id);
// The public mode rotation (`--mode rotation`, server.cfg `modes rotation`) of `flavor`: Krunker's
// g_rotation_modes, or the modes Strike's menu offers. Returns the count; *out the list.
size_t flavor_rotation_modes(GameFlavor flavor, const int **out);
// The default lobby config a game starts from: g_default_game_config, or Strike's
// (strike_game_config(): its match minutes, strafe speed, no streaks, team select, cap).
const GameConfig *flavor_default_game_config(void);
// A serverConfig option's default under the flavor (game_config.c option defaults, the hosted /
// custom config path): 1 and *def when the flavor sets that option's default, else 0.
int flavor_config_default(const char *var, float *def);
// The public lobby cap of the flavor, 0 = the server's own default (Strike: 10).
int flavor_max_players(void);
