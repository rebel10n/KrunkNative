// The server's billboard ad and v7.1.4 utils.js createCanvasAD: client/include/billboard_ad.h.
#include <billboard_ad.h>
#include <stdio.h>
#include <string.h>

static struct {
    BillboardAd staged; // since the last INIT
    int staged_any;
    BillboardAd active;
    int active_set;
} g_ad;

static void ad_copy(char *dst, const size_t size, const char *src, const char *def) {
    snprintf(dst, size, "%s", src && src[0] ? src : def);
}

// game.updateBillboards: txt, txtCol || "#e3e3e3", bgCol || "#000000"
void billboard_ad_on_packet(const NetBillboardPacket *packet) {
    NetBillboardPacket p = *packet;
    p.txt[sizeof(p.txt) - 1] = 0;
    p.txt_col[sizeof(p.txt_col) - 1] = 0;
    p.bg_col[sizeof(p.bg_col) - 1] = 0;
    ad_copy(g_ad.staged.txt, sizeof(g_ad.staged.txt), p.txt, "");
    ad_copy(g_ad.staged.txt_col, sizeof(g_ad.staged.txt_col), p.txt_col, "#e3e3e3");
    ad_copy(g_ad.staged.bg_col, sizeof(g_ad.staged.bg_col), p.bg_col, "#000000");
    g_ad.staged_any = 1;
}

void billboard_ad_on_image_packet(const NetBillboardImagePacket *packet) {
    char path[NET_BILLBOARD_IMAGE_MAX];
    memcpy(path, packet->path, sizeof(path));
    path[sizeof(path) - 1] = 0;
    if (!billboard_image_path_ok(path)) {
        fprintf(stderr, "[billboard] ignoring image '%s' (not an asset path)\n", path);
        return;
    }
    snprintf(g_ad.staged.image, sizeof(g_ad.staged.image), "%s", path);
    g_ad.staged_any = 1;
}

void billboard_ad_on_init(void) {
    g_ad.active = g_ad.staged;
    g_ad.active_set = g_ad.staged_any && (g_ad.active.txt[0] || g_ad.active.image[0]);
    if (g_ad.active_set) {
        if (!g_ad.active.txt_col[0]) snprintf(g_ad.active.txt_col, sizeof(g_ad.active.txt_col), "#e3e3e3");
        if (!g_ad.active.bg_col[0]) snprintf(g_ad.active.bg_col, sizeof(g_ad.active.bg_col), "#000000");
    }
    memset(&g_ad.staged, 0, sizeof(g_ad.staged));
    g_ad.staged_any = 0;
}

const BillboardAd *billboard_ad_active(void) {
    return g_ad.active_set ? &g_ad.active : NULL;
}

int billboard_ad_is_surface(const Object *object) {
    return object && object->prefab == PREFAB_BILLBOARD;
}

const BillboardAd *billboard_ad_for_object(const Object *object) {
    return billboard_ad_is_surface(object) ? billboard_ad_active() : NULL;
}

// utils.js drawMultilineText(ctx, text, txtCol, w, h, scale 1), its layout half. The font
// grows from 15 px until the first word or the wrapped block no longer fits w - 80 / h - 80;
// the lines of the last size that fitted are drawn at that size but positioned with the size
// that failed (v3755 after the loop), exactly like the JS.
void billboard_ad_layout(Canvas2D *c, const char *text, const int width, const int height, BillboardAdLayout *out) {
    memset(out, 0, sizeof(*out));
    const char *words[BILLBOARD_AD_MAX_LINES];
    size_t lengths[BILLBOARD_AD_MAX_LINES];
    int word_count = 0;
    const char *source = text ? text : "Your Ad Here"; // isString(text) ? text : "Your Ad Here"
    for (const char *w = source; word_count < BILLBOARD_AD_MAX_LINES;) { // text.split(" ")
        const char *space = strchr(w, ' ');
        lengths[word_count] = space ? (size_t) (space - w) : strlen(w);
        words[word_count++] = w;
        if (!space) break;
        w = space + 1;
    }

    const double max_w = (double) width - BILLBOARD_AD_PAD;
    const double max_h = (double) height - BILLBOARD_AD_PAD;
    char line[256], joined[256];
    int lines_from[BILLBOARD_AD_MAX_LINES], lines_to[BILLBOARD_AD_MAX_LINES], fit_from[BILLBOARD_AD_MAX_LINES], fit_to[BILLBOARD_AD_MAX_LINES];
    int fit_count = 0, best = 0, size;

    for (size = 15; size <= 2000; size++) {
        const double line_h = size * 1.4;
        double block_h = line_h;
        int count = 0, first = 0;
        c2d_set_font(c, size, "GameFont");
        snprintf(line, sizeof(line), "%.*s", (int) lengths[0], words[0]);
        if (c2d_measure_text(c, line) > max_w) {
            best = size - 1;
            break;
        }
        for (int i = 1; i < word_count; i++) {
            snprintf(joined, sizeof(joined), "%s %.*s", line, (int) lengths[i], words[i]);
            if (c2d_measure_text(c, joined) < max_w) {
                snprintf(line, sizeof(line), "%s", joined);
            } else {
                lines_from[count] = first;
                lines_to[count++] = i;
                first = i;
                snprintf(line, sizeof(line), "%.*s", (int) lengths[i], words[i]);
                block_h += line_h;
            }
        }
        lines_from[count] = first;
        lines_to[count++] = word_count;
        if (block_h > max_h) {
            best = size - 1;
            break;
        }
        memcpy(fit_from, lines_from, sizeof(fit_from));
        memcpy(fit_to, lines_to, sizeof(fit_to));
        fit_count = count;
        best = size;
    }

    out->font_px = best;
    out->layout_px = size;
    out->line_count = fit_count;
    if (!fit_count) return;

    const double line_h = size * 1.4;
    double x = width / 2.0, y = height / 2.0;
    if (fit_count > 1) y -= line_h * (fit_count - 1) / 2.0;
    x += BILLBOARD_AD_PAD / 2.0 / fit_count;
    y += BILLBOARD_AD_PAD / fit_count;
    c2d_set_font(c, best, "GameFont");
    for (int i = 0; i < fit_count; i++) {
        // the words of the line, joined by single spaces, then trim()
        char *dst = out->lines[i];
        size_t n = 0;
        for (int w = fit_from[i]; w < fit_to[i]; w++) {
            n += (size_t) snprintf(dst + n, sizeof(out->lines[i]) - n, "%s%.*s", w > fit_from[i] ? " " : "", (int) lengths[w], words[w]);
            if (n >= sizeof(out->lines[i])) n = sizeof(out->lines[i]) - 1;
        }
        char *start = dst;
        while (*start == ' ') start++;
        memmove(dst, start, strlen(start) + 1);
        n = strlen(dst);
        while (n && dst[n - 1] == ' ') dst[--n] = 0;
        out->x[i] = x - c2d_measure_text(c, dst) / 2.0;
        out->y[i] = y + size / 2.0;
        y += line_h;
    }
}

void billboard_ad_draw(Canvas2D *c, const BillboardAd *ad) {
    const int width = c2d_width(c), height = c2d_height(c);
    float rgba[4] = {0.0f, 0.0f, 0.0f, 1.0f};
    if (!c2d_parse_color(ad->bg_col, rgba)) c2d_parse_color("#000000", rgba);
    c2d_set_fill_color(c, rgba[0], rgba[1], rgba[2], rgba[3]);
    c2d_fill_rect(c, 0.0, 0.0, width, height);

    static BillboardAdLayout layout; // ~4 KB of lines: not on the (web) stack
    billboard_ad_layout(c, ad->txt, width, height, &layout);
    if (!c2d_parse_color(ad->txt_col, rgba)) c2d_parse_color("#e3e3e3", rgba);
    c2d_set_fill_color(c, rgba[0], rgba[1], rgba[2], rgba[3]);
    c2d_set_stroke_color(c, 0.0f, 0.0f, 0.0f, 0.5f);
    c2d_set_line_width(c, 1.0);
    c2d_set_font(c, layout.font_px, "GameFont");
    for (int i = 0; i < layout.line_count; i++) {
        c2d_stroke_text(c, layout.lines[i], layout.x[i], layout.y[i], 0, 0.0);
        c2d_fill_text(c, layout.lines[i], layout.x[i], layout.y[i], 0, 0.0);
    }
}
