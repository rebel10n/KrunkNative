// settings_defaults_test: the v10.0.2 defaults that differ on a phone (client/src/ui/ui_settings_defaults.c).
// Each key of the phone table holds v10's desktop value and its isMobile value; the Mobile rows hold the
// same value on both platforms; keys outside the table keep the captured default (NULL).
#include <ui_settings_defaults.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures, checks;
#define CHECK(c, ...) do { checks++; if (!(c)) { failures++; fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__); \
    fprintf(stderr, __VA_ARGS__); fputc('\n', stderr); } } while (0)

typedef struct {
    const char *key;
    const char *desktop, *phone;
} Expect;

// Display, Game and Interface values v10 starts a phone with (the isMobile column of settings.js)
static const Expect PHONE[] = {
    {"netRate", "3", "1"},
    {"resolution", "0.6", "0.4"},
    {"mapDet", "true", "false"},
    {"shadows", "true", "false"},
    {"muzzleFlash", "true", "false"},
    {"lighting", "1", "0"},
    {"postProcessing", "true", "false"},
    {"reflection", "0", "2"},
    {"showPfp", "true", "false"},
    {"scaleUI", "0.7", "0.9"},
    {"oldScoreboard", "false", "true"},
    {"sensitivityX", "1", "1.4"},
    {"sensitivityY", "1", "1.4"},
    {"aimSensitivityX", "1", "1.4"},
    {"aimSensitivityY", "1", "1.4"},
    {"mouseFlick", "false", "true"},
};

// Game > Mobile: the same on both platforms (v10's defaults for the rows hidden on a desktop)
static const Expect MOBILE[] = {
    {"sensitivityXMbl", "1.4", "1.4"},
    {"sensitivityYMbl", "1.4", "1.4"},
    {"aimSensitivityXMbl", "1.4", "1.4"},
    {"aimSensitivityYMbl", "1.4", "1.4"},
    {"invertYMbl", "false", "false"},
    {"vibrationMbl", "true", "true"},
    {"gradSpeedMbl", "true", "true"},
};

// Box Office's own default (ui_settings_defaults.c): v10 ships rawMouse off, the Client tab's Raw Input was on
static const Expect BOX_OFFICE[] = {
    {"rawMouse", "true", "true"},
};

static const char *text_of(const char *key, const int mobile) { return ui_settings_default_text(key, mobile); }

int main(void) {
    for (size_t i = 0; i < sizeof(PHONE) / sizeof(PHONE[0]); i++) {
        const Expect *e = &PHONE[i];
        const char *desktop = text_of(e->key, 0), *phone = text_of(e->key, 1);
        CHECK(desktop && phone, "%s: missing from the table", e->key);
        if (!desktop || !phone) continue;
        CHECK(!strcmp(desktop, e->desktop), "%s desktop %s, want %s", e->key, desktop, e->desktop);
        CHECK(!strcmp(phone, e->phone), "%s phone %s, want %s", e->key, phone, e->phone);
        CHECK(strcmp(desktop, phone) != 0, "%s: a phone default must differ from the desktop one", e->key);
    }

    for (size_t i = 0; i < sizeof(MOBILE) / sizeof(MOBILE[0]); i++) {
        const Expect *e = &MOBILE[i];
        const char *desktop = text_of(e->key, 0), *phone = text_of(e->key, 1);
        CHECK(desktop && phone, "%s: missing from the table", e->key);
        if (!desktop || !phone) continue;
        CHECK(!strcmp(desktop, e->desktop), "%s desktop %s, want %s", e->key, desktop, e->desktop);
        CHECK(!strcmp(phone, e->phone), "%s phone %s, want %s", e->key, phone, e->phone);
    }

    for (size_t i = 0; i < sizeof(BOX_OFFICE) / sizeof(BOX_OFFICE[0]); i++) {
        const Expect *e = &BOX_OFFICE[i];
        const char *desktop = text_of(e->key, 0), *phone = text_of(e->key, 1);
        CHECK(desktop && phone, "%s: missing from the table", e->key);
        if (!desktop || !phone) continue;
        CHECK(!strcmp(desktop, e->desktop), "%s desktop %s, want %s", e->key, desktop, e->desktop);
        CHECK(!strcmp(phone, e->phone), "%s phone %s, want %s", e->key, phone, e->phone);
    }

    // keys outside the table: the captured (scraped) default stands on both platforms
    CHECK(text_of("fov", 0) == NULL && text_of("fov", 1) == NULL, "fov is not a phone default");
    CHECK(text_of("lowSpec", 1) == NULL, "lowSpec keeps its captured default");
    CHECK(text_of("particles", 1) == NULL, "particles keeps its captured default");
    CHECK(text_of("sensitivityXCntrl", 1) == NULL, "the controller sensitivity keeps its default");
    CHECK(text_of(NULL, 1) == NULL && text_of("", 0) == NULL, "no key, no default");

    // the table is well formed: unique keys, typed values that parse
    size_t n = 0;
    const UiSettingDefault *table = ui_settings_defaults_table(&n);
    CHECK(table && n == sizeof(PHONE) / sizeof(PHONE[0]) + sizeof(MOBILE) / sizeof(MOBILE[0]) +
              sizeof(BOX_OFFICE) / sizeof(BOX_OFFICE[0]), "table holds %zu entries", n);
    for (size_t i = 0; table && i < n; i++) {
        const UiSettingDefault *a = &table[i];
        CHECK(a->key && a->desktop && a->phone, "entry %zu is incomplete", i);
        if (!a->key || !a->desktop || !a->phone) continue;
        for (size_t j = i + 1; j < n; j++) CHECK(strcmp(a->key, table[j].key) != 0, "%s listed twice", a->key);
        if (a->kind == UI_DEFAULT_BOOL) {
            const int bool_text = (!strcmp(a->desktop, "true") || !strcmp(a->desktop, "false")) &&
                (!strcmp(a->phone, "true") || !strcmp(a->phone, "false"));
            CHECK(bool_text, "%s: a boolean default is true or false", a->key);
        } else if (a->kind == UI_DEFAULT_NUMBER) {
            char *end = NULL;
            strtod(a->desktop, &end);
            CHECK(end && *end == 0, "%s: desktop %s is not a number", a->key, a->desktop);
            end = NULL;
            strtod(a->phone, &end);
            CHECK(end && *end == 0, "%s: phone %s is not a number", a->key, a->phone);
        }
    }

    if (failures) {
        fprintf(stderr, "settings_defaults_test: %d of %d checks failed\n", failures, checks);
        return 1;
    }
    printf("settings_defaults_test: %d checks passed\n", checks);
    return 0;
}
