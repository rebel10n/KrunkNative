// Nettspend class audio (client/include/nettspend_audio.h): the sound side of the Purple Drank,
// Money Spread and the laced state, on top of audio.c. The event hooks are stateless one-shots
// (sound_events.c decides when an event happened from its player-state diffs, as for Krunker's
// own sounds); only the laced ambience keeps state (its end time, by sound_events' clock).
#include <nettspend_audio.h>
#include <audio.h>
#include <pcg_basic.h>
#include <stdint.h>
#include <string.h>
#include <string.h>

static struct {
    float now;         // nettspend_audio_tick's clock
    float laced_until; // the laced loop ends at this time, 0 = none
    int laced;         // the loop is playing
} g_nettspend;

static float rate_jitter(const float min, const float max) {
    return min + (max - min) * (float) ((double) pcg32_random() / (double) UINT32_MAX);
}

int nettspend_is_drank(const int weapon_id) {
    return weapon_id == WEAPON_ID_DRANK;
}

int nettspend_is_money_spread(const int weapon_id) {
    return weapon_id == WEAPON_ID_MONEY_SPREAD;
}

// The pour up: the cup is poured (pour) while the player screams and shouts the line (reload);
// both 2D like Krunker's reload_1 / reload_2. The line's rate is jittered a little so two reloads
// in a row do not sound sampled.
int nettspend_audio_reload_start(const int weapon_id) {
    if (!nettspend_is_drank(weapon_id)) return 0;
    audio_play(NETTSPEND_SOUND_POUR, NETTSPEND_POUR_VOLUME, 0, rate_jitter(0.95f, 1.05f));
    audio_play(NETTSPEND_SOUND_RELOAD, NETTSPEND_RELOAD_VOLUME, 0, rate_jitter(0.96f, 1.04f));
    return 1;
}

int nettspend_audio_shot(const int weapon_id) {
    if (!nettspend_is_money_spread(weapon_id)) return 0;
    audio_play(NETTSPEND_SOUND_MONEY, NETTSPEND_MONEY_VOLUME, 0, rate_jitter(0.92f, 1.08f));
    return 1;
}

int nettspend_audio_remote_shot(const int weapon_id, const vec3 position) {
    if (!nettspend_is_money_spread(weapon_id)) return 0;
    audio_play_3d(NETTSPEND_SOUND_MONEY, position, NETTSPEND_REMOTE_MONEY_VOLUME, rate_jitter(0.92f, 1.08f), 0.0f, 0.0f);
    return 1;
}

static void laced_stop(void) {
    if (g_nettspend.laced) audio_stop(NETTSPEND_SOUND_LACED);
    g_nettspend.laced = 0;
    g_nettspend.laced_until = 0.0f;
}

void nettspend_audio_laced(const float duration_s) {
    if (duration_s <= 0.0f) {
        laced_stop();
        return;
    }
    g_nettspend.laced_until = g_nettspend.now + duration_s;
    if (!g_nettspend.laced) {
        g_nettspend.laced = 1;
        audio_play(NETTSPEND_SOUND_LACED, NETTSPEND_LACED_VOLUME, 1, 1.0f); // a loop: not restarted while playing
    }
}


int nettspend_audio_on_laced(const void *body, const unsigned body_length, const int my_uid) {
    NetLacedPacket packet;
    if (!body || body_length < sizeof(packet)) return 0;
    memcpy(&packet, body, sizeof(packet));
    if (packet.uid != my_uid) return 0;
    nettspend_audio_laced((float) packet.duration_ms / 1000.0f);
    return 1;
}

const char *nettspend_audio_stream_sound(const int weapon_id) {
    return nettspend_is_drank(weapon_id) ? NETTSPEND_SOUND_SPLASH : NULL;
}

int nettspend_audio_laced_active(void) {
    return g_nettspend.laced;
}

void nettspend_audio_tick(const float now) {
    g_nettspend.now = now;
    if (g_nettspend.laced && now >= g_nettspend.laced_until) laced_stop();
}

void nettspend_audio_reset(void) {
    laced_stop();
}

void nettspend_audio_preload(void) {
    audio_preload(NETTSPEND_SOUND_RELOAD);
    audio_preload(NETTSPEND_SOUND_POUR);
    audio_preload(NETTSPEND_SOUND_SPLASH);
    audio_preload(NETTSPEND_SOUND_MONEY);
    audio_preload(NETTSPEND_SOUND_LACED);
}
