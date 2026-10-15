// audio_volume_test: the volume sliders reach their sounds (client/src/audio.c, no engine needed):
// each v10 Audio setting (sound, voiceVolume, ambientVolume, gunsVolume, playerVolume, uiVolume,
// assetVolume, dialogueVolume, skinVolume) sets its SOUND.volumes slot, and SOUND.getVolume of a sound in
// that category (v10.js:193556 soundCats) follows it; a sound soundCats does not list stays at 1.
//   audio_volume_test
#include <audio.h>
#include <math.h>
#include <stdio.h>

static int g_failures, g_checks;
#define CHECK(cond, ...) do { g_checks++; if (!(cond)) { g_failures++; printf("FAIL: "); printf(__VA_ARGS__); printf("\n"); } } while (0)

void voice_audio_fini(void) {} // audio.c's one link into voice chat (client/src/voice.c): not under test

static int near(const float a, const float b) { return fabsf(a - b) < 1e-4f; }

int main(void) {
    static const struct {
        const char *key, *sound; // the setting, a sound of its category
        float value;
    } CASES[] = {
        {"dialogueVolume", "medal_0", 0.25f},  // 7: medals, bomb plant / defuse, zone calls, Zombies power-ups
        {"dialogueVolume", "bo_plant", 0.25f}, // (v3.1.1 had bo_plant under master)
        {"dialogueVolume", "instantkill_0", 0.25f},
        {"skinVolume", "shoe_1", 0.5f},        // 8: cosmetic step sounds
        {"skinVolume", "inspect_monte_carlo_spin", 0.5f},
        {"gunsVolume", "weapon_2", 0.4f},
        {"playerVolume", "step_0", 0.3f},
        {"uiVolume", "tick_0", 0.6f},
        {"ambientVolume", "ambient_19", 0.7f},
        {"sound", "nuke_0", 0.9f},             // category 0 is the master slot
    };
    for (size_t i = 0; i < sizeof(CASES) / sizeof(CASES[0]); i++) {
        char value[16];
        snprintf(value, sizeof(value), "%g", CASES[i].value);
        CHECK(audio_apply_setting(CASES[i].key, value) == 1, "%s is an audio setting", CASES[i].key);
        CHECK(near(audio_sound_volume(CASES[i].sound), CASES[i].value), "%s=%g: %s plays at %g", CASES[i].key,
              CASES[i].value, CASES[i].sound, audio_sound_volume(CASES[i].sound));
    }
    CHECK(near(audio_get_volume(AUDIO_CAT_DIALOGUE), 0.25f) && near(audio_get_volume(AUDIO_CAT_SKIN), 0.5f),
          "dialogue / skin slots 7 / 8");
    CHECK(near(audio_sound_volume("shoe_3"), 1.0f), "a shoe soundCats does not list plays at 1 (%g)", audio_sound_volume("shoe_3"));
    // the dialogue slider leaves the other categories alone
    audio_apply_setting("dialogueVolume", "0");
    CHECK(near(audio_sound_volume("medal_1"), 0.0f) && near(audio_sound_volume("shoe_2"), 0.5f) &&
          near(audio_sound_volume("weapon_2"), 0.4f), "dialogueVolume 0 mutes medal_1 only");
    printf("%d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
