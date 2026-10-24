// Krunker v10.0.3 setting defaults that the captured settings tabs cannot carry: the `val` of a
// SETTINGS.list entry (deob-10.0.3.js:236845-241193) that depends on isMobile, or that belongs to a
// control v10 fills at run time or never renders. Each row lists the desktop and the phone value so
// the table reads as v10's; ui_settings.c applies the one for the platform over the scraped tabs.
//
// Checked 2026-10-24: every desktop `val` of the 292 settings in deob-10.0.3.js matches the scrape of
// ui/windows/settings-advanced-*.html plus this table and ui_settings.c COLOR_SETTINGS, except the
// deliberate rawMouse below and defaultRegion (its `val` getter reads the ping cache,
// deob-10.0.3.js:236852; ui_client_settings.c fills that row and servers_ui.c reads only a saved
// value). 10.0.3 changed no default over 10.0.2: it only added noVideoSkins (false, in the capture).
//
// Only v10 values belong here. The KrunkNative Beautiful preset (ui_settings.c BEAUTIFUL) is not a
// v10 setting, and it is hidden on phones (sync_preset_boxes), as v10 has four presets.
#include <ui_settings_defaults.h>

#include <string.h>

static const UiSettingDefault DEFAULTS[] = {
    // Advanced > General: the phone's Network Rate is 60 Hz (index 1), the desktop's 144 Hz (3)
    // (deob-10.0.3.js:236886)
    {"netRate", UI_DEFAULT_TEXT, "3", "1"},
    // Display: Resolution 0.6 -> 0.4, Map Details / Shadows / Muzzle Flash / Post Processing off, the
    // Low lighting, Map Dependent reflections (0) -> 64 px (2), no profile picture
    // (deob-10.0.3.js:237083, 237204, 237286, 237388, 237567, 237599, 237264, 237681)
    {"resolution", UI_DEFAULT_NUMBER, "0.6", "0.4"},
    {"mapDet", UI_DEFAULT_BOOL, "true", "false"},
    {"shadows", UI_DEFAULT_BOOL, "true", "false"},
    {"muzzleFlash", UI_DEFAULT_BOOL, "true", "false"},
    {"lighting", UI_DEFAULT_TEXT, "1", "0"},
    {"postProcessing", UI_DEFAULT_BOOL, "true", "false"},
    {"reflection", UI_DEFAULT_TEXT, "0", "2"},
    {"showPfp", UI_DEFAULT_BOOL, "true", "false"},
    // Interface: the UI scale 0.7 -> 0.9 and the classic scoreboard (the kill feed in the chat)
    // (deob-10.0.3.js:237610, 237654)
    {"scaleUI", UI_DEFAULT_NUMBER, "0.7", "0.9"},
    {"oldScoreboard", UI_DEFAULT_BOOL, "false", "true"},
    // Game: the look sensitivities 1 -> 1.4 (v10's isMobile), and the hidden "Mouse flick fix"
    // (deob-10.0.3.js:239286-239332, 236983)
    {"sensitivityX", UI_DEFAULT_NUMBER, "1", "1.4"},
    {"sensitivityY", UI_DEFAULT_NUMBER, "1", "1.4"},
    {"aimSensitivityX", UI_DEFAULT_NUMBER, "1", "1.4"},
    {"aimSensitivityY", UI_DEFAULT_NUMBER, "1", "1.4"},
    {"mouseFlick", UI_DEFAULT_BOOL, "false", "true"},
    // Game > Mobile: the same defaults on a desktop (their rows only show on a phone)
    {"sensitivityXMbl", UI_DEFAULT_NUMBER, "1.4", "1.4"},
    {"sensitivityYMbl", UI_DEFAULT_NUMBER, "1.4", "1.4"},
    {"aimSensitivityXMbl", UI_DEFAULT_NUMBER, "1.4", "1.4"},
    {"aimSensitivityYMbl", UI_DEFAULT_NUMBER, "1.4", "1.4"},
    {"invertYMbl", UI_DEFAULT_BOOL, "false", "false"},
    {"vibrationMbl", UI_DEFAULT_BOOL, "true", "true"},
    {"gradSpeedMbl", UI_DEFAULT_BOOL, "true", "true"},
    // Sound > Voice Chat: the microphone select is empty in the capture (v10 fills its options from
    // getAudioInputs at run time); its val is "default" (deob-10.0.3.js:239694), the system device
    {"audioInput", UI_DEFAULT_TEXT, "default", "default"},
    // Controls: the hidden Trigger selects the Change Controls window draws next to the In-Game
    // Leaderboard and Voice binds (hide + noShow, so no settings tab has them): Continuous (Hold), 1
    // (deob-10.0.3.js:241139, 241154)
    {"scoreBoardTrigger", UI_DEFAULT_NUMBER, "1", "1"},
    {"voiceChatTrigger", UI_DEFAULT_NUMBER, "1", "1"},
    // Box Office, not v10 (deob-10.0.3.js:236956 ships rawMouse off): raw mouse motion stays on, as the
    // Client tab's Raw Input was (ui_client_settings.c migrate_raw_input carries a stored value over)
    {"rawMouse", UI_DEFAULT_BOOL, "true", "true"},
};

const UiSettingDefault *ui_settings_defaults_table(size_t *count) {
    if (count) *count = sizeof(DEFAULTS) / sizeof(DEFAULTS[0]);
    return DEFAULTS;
}

const char *ui_settings_default_text(const char *key, const int mobile) {
    if (!key) return NULL;
    for (size_t i = 0; i < sizeof(DEFAULTS) / sizeof(DEFAULTS[0]); i++) {
        if (!strcmp(DEFAULTS[i].key, key)) return mobile ? DEFAULTS[i].phone : DEFAULTS[i].desktop;
    }
    return NULL;
}
