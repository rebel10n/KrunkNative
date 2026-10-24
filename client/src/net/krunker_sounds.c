// The official link's sound table (client/include/krunker_sounds.h): config.networkSounds, the
// shared v10.0.3 table (shared/src/rules/v10/network_sounds_v10.c).
#include <krunker_sounds.h>
#include <network_sounds.h>

const char *krunker_sound_name(const int index) { return network_sound_name(index); }

int krunker_sound_count(void) { return NETWORK_SOUND_COUNT; }
