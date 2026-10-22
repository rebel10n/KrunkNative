// The paint badge of an owned skin card (client/src/ui/skin_paint.c) and the Video Skins row of the
// Render tab (ui/windows/settings-advanced-3.html), against Krunker v10.0.3's markup and rules.
//   cmake-build-debug/skin_paint_test [repo-root]     (the root holds ui/ and client/; default ".")
#include <skin_paint.h>
#include <i18n.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_fail, g_pass;
#define CHECK(cond, ...) do { if (cond) g_pass++; else { g_fail++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

// ---- test doubles: the English of the one key the badge translates (the page's en.json is not loaded here)
const char *i18n_trf(char *out, size_t n, const char *key, const char *english, int argc, const char *const *argv) {
    (void) key;
    size_t o = 0;
    for (const char *p = english; *p && o + 1 < n; ) {
        if (argc > 0 && !strncmp(p, "{0}", 3)) {
            for (const char *a = argv[0]; *a && o + 1 < n; a++) out[o++] = *a;
            p += 3;
        } else {
            out[o++] = *p++;
        }
    }
    out[o] = '\0';
    return out;
}

// the whole file at root/rel (NULL when it cannot be read)
static char *read_file(const char *root, const char *rel) {
    char path[1024];
    snprintf(path, sizeof(path), "%s/%s", root, rel);
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    const long len = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *buf = len > 0 ? malloc((size_t) len + 1) : NULL;
    if (buf) {
        const size_t got = fread(buf, 1, (size_t) len, f);
        buf[got] = '\0';
    }
    fclose(f);
    return buf;
}

// the badge for meta / store into a buffer, as the card builder calls it
static void badge(char *out, size_t n, const char *json, const SkinPaint *store) {
    cJSON *meta = json ? cJSON_Parse(json) : NULL;
    skin_paint_badge(out, n, meta, store);
    cJSON_Delete(meta);
}

static void test_shows(void) {
    cJSON *none = cJSON_Parse("{}");
    cJSON *paint = cJSON_Parse("{\"paint\":\"#dc3c3c\"}");
    cJSON *shader = cJSON_Parse("{\"paintShader\":3}");
    cJSON *shader_null = cJSON_Parse("{\"paintShader\":null,\"paint\":\"\"}");
    CHECK(!skin_paint_shows(NULL), "NULL metadata shows no badge");
    CHECK(!skin_paint_shows(none), "empty metadata shows no badge");
    CHECK(skin_paint_shows(paint), "a paint colour shows the badge");
    CHECK(skin_paint_shows(shader), "a paint shader shows the badge");
    CHECK(!skin_paint_shows(shader_null), "a null shader and an empty paint show no badge");
    CHECK(skin_paint_color(paint) && !strcmp(skin_paint_color(paint), "#dc3c3c"), "the paint colour is read from metadata.paint");
    CHECK(skin_paint_color(shader) == NULL, "a shader-only item has no paint colour");
    CHECK(skin_paint_same_color("#DC3C3C", "#dc3c3c") == 1, "colours compare case-insensitively");
    CHECK(skin_paint_same_color("#dc3c3c", "#50c864") == 0, "different colours do not match");
    CHECK(skin_paint_same_color(NULL, "#dc3c3c") == 0, "no colour matches nothing");
    cJSON_Delete(none);
    cJSON_Delete(paint);
    cJSON_Delete(shader);
    cJSON_Delete(shader_null);
}

static void test_badge_markup(void) {
    char out[1024];
    SkinPaint red = {"Red Paint", "textures/paints/paint_red.webp"};
    SkinPaint red_plain = {"Red Paint", NULL};

    badge(out, sizeof(out), NULL, NULL);
    CHECK(out[0] == '\0', "no metadata: no badge markup");
    badge(out, sizeof(out), "{}", &red);
    CHECK(out[0] == '\0', "an unpainted item: no badge markup, store paint or not");

    badge(out, sizeof(out), "{\"paint\":\"#DC3C3C\"}", &red);
    CHECK(!strcmp(out, "<div class='skinPaintBadge' title='Painted: Red Paint'><img draggable='false' src='textures/paints/paint_red.webp' /></div>"),
          "a store paint with a thumbnail shows the thumbnail: %s", out);

    badge(out, sizeof(out), "{\"paint\":\"#DC3C3C\"}", &red_plain);
    CHECK(!strcmp(out, "<div class='skinPaintBadge' title='Painted: Red Paint'><span class='skinPaintSwatch' style='background-color:#dc3c3c'></span></div>"),
          "a store paint without a thumbnail shows the lowercase colour swatch: %s", out);

    badge(out, sizeof(out), "{\"paint\":\"#DC3C3C\"}", NULL);
    CHECK(!strcmp(out, "<div class='skinPaintBadge' title='Painted: #dc3c3c'><span class='skinPaintSwatch' style='background-color:#dc3c3c'></span></div>"),
          "no store paint: the title is the lowercase colour: %s", out);

    badge(out, sizeof(out), "{\"paintShader\":3}", NULL);
    CHECK(!strcmp(out, "<div class='skinPaintBadge' title='Painted: Silver Paint'><i class='material-icons'>format_paint</i></div>"),
          "a shader with no store paint passed: the title is its store paint, Silver Paint, and the format_paint icon: %s", out);
    badge(out, sizeof(out), "{\"paintShader\":9}", NULL);
    CHECK(strstr(out, "title='Painted: Shader #9'") != NULL, "a number that is no shader keeps Shader #9: %s", out);

    SkinPaint odd = {"Gold \"Ore\" & <x>", NULL};
    badge(out, sizeof(out), "{\"paintShader\":1,\"paint\":\"#ffd700\"}", &odd);
    CHECK(strstr(out, "title='Painted: Gold &quot;Ore&quot; &amp; &lt;x&gt;'") != NULL, "the store name is escaped in the title: %s", out);
    CHECK(strstr(out, "<span class='skinPaintSwatch' style='background-color:#ffd700'></span>") != NULL,
          "a colour paint keeps its swatch when the store has no thumbnail: %s", out);

    // a tiny buffer never overruns: the markup is cut, still NUL-terminated
    char small[16];
    cJSON *meta = cJSON_Parse("{\"paint\":\"#dc3c3c\"}");
    const size_t written = skin_paint_badge(small, sizeof(small), meta, &red);
    CHECK(written < sizeof(small) && small[written] == '\0', "a small buffer is cut and terminated");
    cJSON_Delete(meta);
}

// ui/windows/settings-advanced-3.html: the Quality folder's Disable Video Skins row, between Disable
// Animated Paints and Screen Shake, a switch that calls setSetting("noVideoSkins") and is off by default.
static void test_settings_row(const char *root) {
    char *html = read_file(root, "ui/windows/settings-advanced-3.html");
    CHECK(html != NULL, "ui/windows/settings-advanced-3.html can be read under %s", root);
    if (html) {
        const char *call = "window.setSetting(&quot;noVideoSkins&quot;, this.checked)";
        const char *at = strstr(html, call);
        CHECK(at != NULL, "the Render tab has a noVideoSkins switch");
        CHECK(at && strstr(at + 1, call) == NULL, "exactly one noVideoSkins switch");
        if (at) {
            const char *row = at;
            while (row > html && strncmp(row, "<div class=\"settName\"", 21)) row--;
            const char *row_end = strstr(at, "</label></div>");
            CHECK(row_end != NULL, "the row ends with its switch");
            if (row_end) {
                const size_t len = (size_t) (row_end - row);
                char *text = malloc(len + 1);
                if (text) {
                    memcpy(text, row, len);
                    text[len] = '\0';
                    CHECK(strstr(text, "title=\"Render video skins with their regular texture instead of the video. Restart required\"") != NULL,
                          "the row carries v10.0.3's info text");
                    CHECK(strstr(text, ">Disable Video Skins <span style=\"color: #eb5656\">*</span> ") != NULL,
                          "the row is labelled Disable Video Skins with the restart mark");
                    CHECK(strstr(text, " checked") == NULL, "the switch is off by default (no checked attribute; this.checked is the handler's)");
                    free(text);
                }
            }
            const char *paints = strstr(html, "setSetting(&quot;noPaintAnim&quot;");
            const char *shake = strstr(html, "setSetting(&quot;screenShake&quot;");
            CHECK(paints && paints < at && at < shake, "the row sits after Disable Animated Paints and before Screen Shake");
        }
        free(html);
    }

    char *en = read_file(root, "ui/i18n/en.json");
    CHECK(en != NULL, "ui/i18n/en.json can be read under %s", root);
    if (en) {
        CHECK(strstr(en, "\"settings.quality.noVideoSkins\": \"Disable Video Skins\"") != NULL,
              "en.json has settings.quality.noVideoSkins = Disable Video Skins");
        CHECK(strstr(en, "\"windows.skins.painted\": \"Painted: {0}\"") != NULL,
              "en.json has windows.skins.painted = Painted: {0}");
        free(en);
    }
}

int main(int argc, char **argv) {
    const char *root = argc > 1 ? argv[1] : ".";
    test_shows();
    test_badge_markup();
    test_settings_row(root);
    printf("skin_paint_test: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
