#pragma once
// The official Krunker client's sound-name array, config.networkSounds: the "s" PLAYER_SOUND /
// "so" PLAY_SOUND / "sso" STOP_SOUND packets carry a sound as an index into it (game.playerSound:
// `isAsset || !isFinite(i) ? i : sounds[i]`). The table is the shared one (shared/include/
// network_sounds.h, v10.0.3's 399 names); these are the official link's names for it. The names
// are assets/sound/<name>.mp3.

// The name at index, NULL past the table.
const char *krunker_sound_name(int index);
int krunker_sound_count(void);
