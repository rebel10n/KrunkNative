// The phone defaults of Krunker v10.0.2 (~/.cache/kn-v10/v10.js, the settings definitions near
// 10011000-10100000: `val` per setting, with isMobile true or false). Each row lists both values so
// the table reads as v10's; ui_settings.c applies the one for the platform over the scraped tabs.
//
// Only v10 values belong here. The KrunkNative Beautiful preset (ui_settings.c BEAUTIFUL) is not a
// v10 setting, and it is hidden on phones (sync_preset_boxes), as v10 has four presets.
#include <ui_settings_defaults.h>

#include <string.h>

static const UiSettingDefault DEFAULTS[] = {
    // Advanced > General: the phone's Network Rate is 60 Hz (index 1), the desktop's 144 Hz (3)
    {"netRate", UI_DEFAULT_TEXT, "3", "1"},
    // Display: Resolution 0.6 -> 0.4, Map Details / Shadows / Muzzle Flash / Post Processing off, the
    // Low lighting, Map Dependent reflections (0) -> 64 px (2), no profile picture
    {"resolution", UI_DEFAULT_NUMBER, "0.6", "0.4"},
    {"mapDet", UI_DEFAULT_BOOL, "true", "false"},
    {"shadows", UI_DEFAULT_BOOL, "true", "false"},
    {"muzzleFlash", UI_DEFAULT_BOOL, "true", "false"},
    {"lighting", UI_DEFAULT_TEXT, "1", "0"},
    {"postProcessing", UI_DEFAULT_BOOL, "true", "false"},
    {"reflection", UI_DEFAULT_TEXT, "0", "2"},
    {"showPfp", UI_DEFAULT_BOOL, "true", "false"},
    // Interface: the UI scale 0.7 -> 0.9 and the classic scoreboard (the kill feed in the chat)
    {"scaleUI", UI_DEFAULT_NUMBER, "0.7", "0.9"},
    {"oldScoreboard", UI_DEFAULT_BOOL, "false", "true"},
    // Game: the look sensitivities 1 -> 1.4 (v10's isMobile), and the hidden "Mouse flick fix"
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
