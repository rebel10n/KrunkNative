#pragma once
// Pure rules of v10.0.7's SETTINGS.set() and of the settings' own set() functions that KrunkNative's
// settings store (ui_settings.c), aim input (ui_controls.c) and frame cap (ui_render_settings.c) share.
// No DOM, no store: client/src/ui/ui_settings_rules.c, tested by client/tests/settings_rules_test.c.
#include <stddef.h>

// crosshairSho / crosshairShoM set() (deob-10.0.7.js:211866-211890, :212126-212150): whether the row of
// `key` (its "<key>_div") is hidden for crosshair type `type` / throwing crosshair type `type_m`.
// -1: not a row these two settings show or hide.
int settings_rule_crosshair_hidden(const char *key, int type, int type_m);
// The keys settings_rule_crosshair_hidden knows, for walking them all (n set to the count).
const char *const *settings_rule_crosshair_rows(size_t *n);

// img settings: /\.(jpg|jpeg|png|gif|webp|ico)/.test(value) (deob-10.0.7.js:214445)
int settings_rule_img_ok(const char *value);
// aspectRatio's testfor /^[-]?\d*x[-]?\d*$/ (deob-10.0.7.js:210442)
int settings_rule_aspect_ok(const char *value);

// toggleADS (deob-10.0.7.js:228303, :228427): CONTROLS.adsToggled after an aim key press / release,
// `toggle` = toggleADS[getPlayerWeaponId(you)].
int settings_rule_ads_press(int toggle, int toggled);
int settings_rule_ads_release(int toggle, int toggled);
// The weapon swap (deob-10.0.7.js:176417-176419): a toggled aim ends when the old weapon toggles and
// the new one holds.
int settings_rule_ads_swap(int toggled, int toggle_old, int toggle_new);

// updateRate.set: `v && v < 10 && (v = 10)` (deob-10.0.7.js:210431)
int settings_rule_update_rate(int v);

// windows[2].import's key clean-up: key.replace(/[^a-zA-Z0-9 _-]+/gim, "") (deob-10.0.7.js:218590)
void settings_rule_loadout_key(const char *in, char *out, size_t n);
