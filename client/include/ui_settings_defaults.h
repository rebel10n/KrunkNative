#pragma once
// Krunker v10.0.2's settings defaults that differ on a phone (settings.js: the `val` of each setting
// is `isMobile ? X : Y`), and the mobile Mobile-category rows, which have the same default on both.
// Pure data, no page or GL dependency: ui_settings.c patches the scraped defaults with the table
// (collect_defaults), client/tests/settings_defaults_test.c checks it.
#include <stddef.h>

typedef enum { UI_DEFAULT_NUMBER, UI_DEFAULT_BOOL, UI_DEFAULT_TEXT } UiDefaultKind;

typedef struct {
    const char *key;
    UiDefaultKind kind;
    const char *desktop; // v10.0.2's value on a desktop, as the setting's control holds it
    const char *phone;   // v10.0.2's value when window._isMobile (CONTROLS.isMobile) is true
} UiSettingDefault;

// The table (count set on return) and its size.
const UiSettingDefault *ui_settings_defaults_table(size_t *count);

// The default text of `key` on a phone (`mobile` 1) or a desktop (0): NULL when the table does not
// list the key, so the scraped default of the captured tab stands.
const char *ui_settings_default_text(const char *key, int mobile);
