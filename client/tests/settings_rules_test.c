// settings_rules_test: the pure v10.0.7 settings rules of client/src/ui/ui_settings_rules.c against the deob's
// expressions (deob-10.0.7.js line numbers in client/include/ui_settings_rules.h).
#include <ui_settings_rules.h>
#include <stdio.h>
#include <string.h>

static int g_failed;
#define CHECK(cond, ...)                                                                                                \
    do {                                                                                                                \
        if (!(cond)) {                                                                                                  \
            g_failed++;                                                                                                 \
            fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__);                                                        \
            fprintf(stderr, __VA_ARGS__);                                                                               \
            fputc('\n', stderr);                                                                                        \
        }                                                                                                               \
    } while (0)

// crosshairSho.set (deob-10.0.7.js:211866-211890) as written there, for every type 0..5
static int sho_hidden(const char *key, const int e) {
    if (!strcmp(key, "crosshairAlways") || !strcmp(key, "crosshairOpacity")) return e < 2;
    if (!strcmp(key, "crosshairLen") || !strcmp(key, "crosshairThick")) return 4 == e || e < 2;
    if (!strcmp(key, "crosshairColor") || !strcmp(key, "crosshairShadow") || !strcmp(key, "crosshairShadowThickess")) return e < 1 || 4 == e;
    if (!strcmp(key, "crosshairImage") || !strcmp(key, "crosshairImageW") || !strcmp(key, "crosshairImageH")) return 4 != e;
    if (!strcmp(key, "crosshairGap")) return 5 != e;
    if (!strcmp(key, "crosshairDot")) return !(5 == e || 1 == e);
    if (!strcmp(key, "crosshairStyle")) return e > 3 || e < 2;
    return -1;
}

// crosshairShoM.set (:212126-212150)
static int sho_m_hidden(const char *key, const int e) {
    if (!strcmp(key, "crosshairAlwaysM") || !strcmp(key, "crosshairOpacityM")) return e < 1;
    if (!strcmp(key, "crosshairLenM") || !strcmp(key, "crosshairThickM")) return 2 == e || e < 1;
    if (!strcmp(key, "crosshairColorM") || !strcmp(key, "crosshairShadowM") || !strcmp(key, "crosshairShadowThickessM")) return e < 1 || 2 == e;
    if (!strcmp(key, "crosshairImageM") || !strcmp(key, "crosshairImageWM") || !strcmp(key, "crosshairImageHM")) return 2 != e;
    if (!strcmp(key, "crosshairGapM") || !strcmp(key, "crosshairDotM")) return 3 != e;
    if (!strcmp(key, "crosshairStyleM")) return 1 != e;
    return -1;
}

static void test_crosshair_rows(void) {
    size_t n = 0;
    const char *const *rows = settings_rule_crosshair_rows(&n);
    CHECK(n == 26, "26 crosshair rows, got %zu", n);
    for (size_t i = 0; i < n; i++) {
        const size_t len = strlen(rows[i]);
        const int throwing = rows[i][len - 1] == 'M';
        for (int t = 0; t <= 5; t++) {
            for (int m = 0; m <= 3; m++) {
                const int want = throwing ? sho_m_hidden(rows[i], m) : sho_hidden(rows[i], t);
                const int got = settings_rule_crosshair_hidden(rows[i], t, m);
                CHECK(want >= 0 && got == want, "%s t=%d m=%d: %d, v10 %d", rows[i], t, m, got, want);
            }
        }
    }
    CHECK(settings_rule_crosshair_hidden("crosshairSho", 1, 3) == -1, "crosshairSho itself is never hidden");
    CHECK(settings_rule_crosshair_hidden("fov", 1, 3) == -1, "fov is not a crosshair row");
    CHECK(settings_rule_crosshair_hidden(NULL, 1, 3) == -1, "NULL key");
    // the defaults (crosshairSho 1, crosshairShoM 3): the dot shows, the image does not
    CHECK(settings_rule_crosshair_hidden("crosshairDot", 1, 3) == 0, "type 1 shows crosshairDot");
    CHECK(settings_rule_crosshair_hidden("crosshairImage", 1, 3) == 1, "type 1 hides crosshairImage");
    CHECK(settings_rule_crosshair_hidden("crosshairGapM", 1, 3) == 0, "throwing type 3 shows crosshairGapM");
}

static void test_values(void) {
    // img: /\.(jpg|jpeg|png|gif|webp|ico)/ (:214445)
    CHECK(settings_rule_img_ok("https://x.y/a.png"), "a .png url");
    CHECK(settings_rule_img_ok("a.webp?x=1"), "a .webp with a query");
    CHECK(settings_rule_img_ok("a.jpeg"), ".jpeg");
    CHECK(!settings_rule_img_ok("https://x.y/a.svg"), ".svg is refused");
    CHECK(!settings_rule_img_ok("png"), "no dot");
    CHECK(!settings_rule_img_ok(""), "empty");
    CHECK(!settings_rule_img_ok(NULL), "NULL");
    // aspectRatio testfor /^[-]?\d*x[-]?\d*$/ (:210442)
    CHECK(settings_rule_aspect_ok("16x9"), "16x9");
    CHECK(settings_rule_aspect_ok("x"), "x alone matches \\d*x\\d*");
    CHECK(settings_rule_aspect_ok("-4x-3"), "signed");
    CHECK(!settings_rule_aspect_ok("16:9"), "16:9");
    CHECK(!settings_rule_aspect_ok("16x9x"), "trailing x");
    CHECK(!settings_rule_aspect_ok(""), "empty");
    // updateRate.set: v && v < 10 && (v = 10) (:210431)
    CHECK(settings_rule_update_rate(0) == 0, "0 = uncapped");
    CHECK(settings_rule_update_rate(1) == 10, "1 -> 10");
    CHECK(settings_rule_update_rate(9) == 10, "9 -> 10");
    CHECK(settings_rule_update_rate(10) == 10, "10 stays");
    CHECK(settings_rule_update_rate(144) == 144, "144 stays");
    CHECK(settings_rule_update_rate(-5) == 10, "-5 -> 10 (truthy and < 10)");
    // the import's loadout key clean-up /[^a-zA-Z0-9 _-]+/gim (:218590)
    char out[32];
    settings_rule_loadout_key("My <Load>out_1-2!", out, sizeof out);
    CHECK(!strcmp(out, "My Loadout_1-2"), "cleaned key %s", out);
    settings_rule_loadout_key("abcdefgh", out, 4);
    CHECK(!strcmp(out, "abc"), "truncated key %s", out);
}

static void test_ads(void) {
    // aim key press (:228303): toggleADS && adsToggled ? false : true
    CHECK(settings_rule_ads_press(0, 0) == 1 && settings_rule_ads_press(0, 1) == 1, "hold: press aims");
    CHECK(settings_rule_ads_press(1, 0) == 1, "toggle: the first press aims");
    CHECK(settings_rule_ads_press(1, 1) == 0, "toggle: the second press stops");
    // release (:228427): toggleADS || (adsToggled = false)
    CHECK(settings_rule_ads_release(0, 1) == 0, "hold: release stops");
    CHECK(settings_rule_ads_release(1, 1) == 1 && settings_rule_ads_release(1, 0) == 0, "toggle: release keeps the state");
    // swap (:176417-176419): a toggled aim ends when the new weapon holds
    CHECK(settings_rule_ads_swap(1, 1, 0) == 0, "toggle -> hold weapon ends the aim");
    CHECK(settings_rule_ads_swap(1, 1, 1) == 1, "toggle -> toggle keeps it");
    CHECK(settings_rule_ads_swap(1, 0, 0) == 1, "not a toggle weapon: unchanged");
    CHECK(settings_rule_ads_swap(0, 1, 0) == 0, "not aiming");
}

int main(void) {
    test_crosshair_rows();
    test_values();
    test_ads();
    if (g_failed) {
        fprintf(stderr, "settings_rules_test: %d failed\n", g_failed);
        return 1;
    }
    printf("settings_rules_test: ok\n");
    return 0;
}
