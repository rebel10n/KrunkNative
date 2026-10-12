#pragma once
// WP-E (doc/v7/map-logic.md §1.9, §4.3, §5.4, §7 WP-E): v7.1.4 custom game config (serverConfig 85
// keys, weps / cls / prks overrides, host types), the map "config" block and the shared
// end-of-game / vote / next-match flow. Declared by WP-A; the bodies in shared/src/game_config.c
// are stubs until WP-E replaces them.
#include <shared.h>

// setupGameConfig + applyConfig(cfg, 3, hostIndex) of a map's embedded "config" (Map.game_config):
// applied when a map is hosted and in the offline sandbox (decisions.md "Embedded map config").
// Returns 1 when something was applied.
int game_apply_map_config(Game*, const Map*);
// endData.mts / maVote / mv: one shared flow for server_bin and the sandbox (§5.4).
void game_next_match(Game*);
int game_vote_match(Game*, Player*, int mode, int map_index); // maVote "mode,map"; 0 = rejected
