// Pure rules of v10.0.7's settings (client/include/ui_settings_rules.h).
#include <ui_settings_rules.h>

#include <ctype.h>
#include <stdio.h>
#include <string.h>

static const char *const CROSSHAIR_ROWS[] = {
    "crosshairAlways", "crosshairOpacity", "crosshairLen", "crosshairThick", "crosshairColor", "crosshairShadow",
    "crosshairShadowThickess", "crosshairImage", "crosshairImageW", "crosshairImageH", "crosshairGap", "crosshairDot",
    "crosshairStyle",
    "crosshairAlwaysM", "crosshairOpacityM", "crosshairLenM", "crosshairThickM", "crosshairColorM", "crosshairShadowM",
    "crosshairShadowThickessM", "crosshairImageM", "crosshairImageWM", "crosshairImageHM", "crosshairGapM", "crosshairDotM",
    "crosshairStyleM",
};

const char *const *settings_rule_crosshair_rows(size_t *n) {
    if (n) *n = sizeof(CROSSHAIR_ROWS) / sizeof(CROSSHAIR_ROWS[0]);
    return CROSSHAIR_ROWS;
}

int settings_rule_crosshair_hidden(const char *key, const int t, const int m) {
    if (!key) return -1;
    // crosshairSho (deob-10.0.7.js:211866-211890)
    if (!strcmp(key, "crosshairAlways") || !strcmp(key, "crosshairOpacity")) return t < 2;
    if (!strcmp(key, "crosshairLen") || !strcmp(key, "crosshairThick")) return t == 4 || t < 2;
    if (!strcmp(key, "crosshairColor") || !strcmp(key, "crosshairShadow") || !strcmp(key, "crosshairShadowThickess"))
        return t < 1 || t == 4;
    if (!strcmp(key, "crosshairImage") || !strcmp(key, "crosshairImageW") || !strcmp(key, "crosshairImageH")) return t != 4;
    if (!strcmp(key, "crosshairGap")) return t != 5;
    if (!strcmp(key, "crosshairDot")) return !(t == 5 || t == 1);
    if (!strcmp(key, "crosshairStyle")) return t > 3 || t < 2;
    // crosshairShoM (deob-10.0.7.js:212126-212150)
    if (!strcmp(key, "crosshairAlwaysM") || !strcmp(key, "crosshairOpacityM")) return m < 1;
    if (!strcmp(key, "crosshairLenM") || !strcmp(key, "crosshairThickM")) return m == 2 || m < 1;
    if (!strcmp(key, "crosshairColorM") || !strcmp(key, "crosshairShadowM") || !strcmp(key, "crosshairShadowThickessM"))
        return m < 1 || m == 2;
    if (!strcmp(key, "crosshairImageM") || !strcmp(key, "crosshairImageWM") || !strcmp(key, "crosshairImageHM")) return m != 2;
    if (!strcmp(key, "crosshairGapM") || !strcmp(key, "crosshairDotM")) return m != 3;
    if (!strcmp(key, "crosshairStyleM")) return m != 1;
    return -1;
}

int settings_rule_img_ok(const char *value) {
    static const char *const EXT[] = {".jpg", ".jpeg", ".png", ".gif", ".webp", ".ico"};
    for (size_t i = 0; value && i < sizeof(EXT) / sizeof(EXT[0]); i++) {
        if (strstr(value, EXT[i])) return 1;
    }
    return 0;
}

int settings_rule_aspect_ok(const char *s) {
    if (!s) return 0;
    if (*s == '-') s++;
    while (isdigit((unsigned char) *s)) s++;
    if (*s++ != 'x') return 0;
    if (*s == '-') s++;
    while (isdigit((unsigned char) *s)) s++;
    return !*s;
}

int settings_rule_ads_press(const int toggle, const int toggled) {
    return !(toggle && toggled); // toggleADS[w] && adsToggled ? adsToggled = false : adsToggled = true
}

int settings_rule_ads_release(const int toggle, const int toggled) {
    return toggle ? toggled : 0; // toggleADS[w] || (adsToggled = false)
}

int settings_rule_ads_swap(const int toggled, const int toggle_old, const int toggle_new) {
    return toggled && toggle_old && !toggle_new ? 0 : toggled;
}

int settings_rule_update_rate(const int v) {
    return v && v < 10 ? 10 : v;
}

void settings_rule_loadout_key(const char *in, char *out, const size_t n) {
    size_t k = 0;
    for (const char *c = in ? in : ""; *c && k + 1 < n; c++) {
        if (isalnum((unsigned char) *c) || *c == ' ' || *c == '_' || *c == '-') out[k++] = *c;
    }
    if (n) out[k] = 0;
}
