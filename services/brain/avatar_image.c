/*
 * services/brain/avatar_image.c - profile picture intake (avatar_image.h).
 */
#include "avatar_image.h"

#include <stdlib.h>
#include <string.h>

#include <stb_image.h> /* client/include: the single-header decoder (the implementation is client/src/stb_image.c) */
#ifdef KN_HAVE_WEBP
#include <webp/decode.h>
#include <webp/encode.h>
#endif

#define AVATAR_WEBP_QUALITY 82.0f

AvatarFormat avatar_sniff(const unsigned char *d, const size_t n) {
    static const unsigned char png[8] = {0x89, 'P', 'N', 'G', 0x0d, 0x0a, 0x1a, 0x0a};
    if (n >= 8 && !memcmp(d, png, 8)) return AVATAR_FMT_PNG;
    if (n >= 3 && d[0] == 0xff && d[1] == 0xd8 && d[2] == 0xff) return AVATAR_FMT_JPEG;
    if (n >= 12 && !memcmp(d, "RIFF", 4) && !memcmp(d + 8, "WEBP", 4)) return AVATAR_FMT_WEBP;
    return AVATAR_FMT_NONE;
}

int avatar_image_available(void) {
#ifdef KN_HAVE_WEBP
    return 1;
#else
    return 0;
#endif
}

void avatar_image_free(AvatarImage *img) {
    if (!img) return;
#ifdef KN_HAVE_WEBP
    WebPFree(img->webp);
#endif
    img->webp = NULL;
    img->webp_len = 0;
}

/* header-only size probe: no pixels are decoded */
static int probe(const AvatarFormat fmt, const unsigned char *src, const size_t n, int *w, int *h) {
    if (fmt == AVATAR_FMT_WEBP) {
#ifdef KN_HAVE_WEBP
        return WebPGetInfo(src, n, w, h) != 0;
#else
        return 0;
#endif
    }
    int comp = 0;
    return stbi_info_from_memory(src, (int) n, w, h, &comp);
}

/* RGBA8 pixels of a whole image: stb_image for PNG / JPEG (4 channels, straight alpha), libwebp for WebP */
static unsigned char *decode(const AvatarFormat fmt, const unsigned char *src, const size_t n, const int w, const int h) {
    if (fmt == AVATAR_FMT_WEBP) {
#ifdef KN_HAVE_WEBP
        int dw = 0, dh = 0;
        uint8_t *px = WebPDecodeRGBA(src, n, &dw, &dh);
        if (px && (dw != w || dh != h)) {
            WebPFree(px);
            return NULL;
        }
        return px;
#else
        (void) w;
        (void) h;
        return NULL;
#endif
    }
    int dw = 0, dh = 0, comp = 0;
    unsigned char *px = stbi_load_from_memory(src, (int) n, &dw, &dh, &comp, 4);
    if (px && (dw != w || dh != h)) {
        stbi_image_free(px);
        return NULL;
    }
    return px;
}

/* Center square of the source, box-filtered down to AVATAR_PX (or nearest-sampled up when the
 * square is smaller). out: AVATAR_PX * AVATAR_PX * 4 bytes. */
static void square_resample(const unsigned char *px, const int w, const int h, unsigned char *out) {
    const int side = w < h ? w : h;
    const int x0 = (w - side) / 2, y0 = (h - side) / 2;
    for (int dy = 0; dy < AVATAR_PX; dy++) {
        int sy0 = y0 + (int) ((long) dy * side / AVATAR_PX);
        int sy1 = y0 + (int) ((long) (dy + 1) * side / AVATAR_PX);
        if (sy1 <= sy0) sy1 = sy0 + 1;
        for (int dx = 0; dx < AVATAR_PX; dx++) {
            int sx0 = x0 + (int) ((long) dx * side / AVATAR_PX);
            int sx1 = x0 + (int) ((long) (dx + 1) * side / AVATAR_PX);
            if (sx1 <= sx0) sx1 = sx0 + 1;
            unsigned long sum[4] = {0, 0, 0, 0};
            unsigned long count = 0;
            for (int y = sy0; y < sy1 && y < h; y++)
                for (int x = sx0; x < sx1 && x < w; x++) {
                    const unsigned char *p = px + ((size_t) y * (size_t) w + (size_t) x) * 4;
                    sum[0] += p[0];
                    sum[1] += p[1];
                    sum[2] += p[2];
                    sum[3] += p[3];
                    count++;
                }
            unsigned char *o = out + ((size_t) dy * AVATAR_PX + (size_t) dx) * 4;
            for (int c = 0; c < 4; c++) o[c] = count ? (unsigned char) ((sum[c] + count / 2) / count) : 0;
        }
    }
}

const char *avatar_image_normalize(const unsigned char *src, const size_t n, AvatarImage *out) {
    memset(out, 0, sizeof(*out));
    if (n > AVATAR_MAX_BYTES) return "FileTooLarge";
    const AvatarFormat fmt = avatar_sniff(src, n);
    if (fmt == AVATAR_FMT_NONE) return "UnsupportedType";
    if (!avatar_image_available()) return "AvatarsUnavailable";
    int w = 0, h = 0;
    if (!probe(fmt, src, n, &w, &h) || w <= 0 || h <= 0) return "BadImage";
    if (w < AVATAR_MIN_SRC_PX || h < AVATAR_MIN_SRC_PX) return "ImageTooSmall";
    if (w > AVATAR_MAX_SRC_PX || h > AVATAR_MAX_SRC_PX) return "ImageTooLarge";
    unsigned char *px = decode(fmt, src, n, w, h);
    if (!px) return "BadImage";
    unsigned char *square = malloc((size_t) AVATAR_PX * AVATAR_PX * 4);
    if (!square) {
        stbi_image_free(px);
        return "BadImage";
    }
    square_resample(px, w, h, square);
    /* the decoded pixels came from stb_image (malloc) or libwebp (WebPDecodeRGBA): free the same way */
    if (fmt == AVATAR_FMT_WEBP) {
#ifdef KN_HAVE_WEBP
        WebPFree(px);
#endif
    } else stbi_image_free(px);
    const char *err = "BadImage";
#ifdef KN_HAVE_WEBP
    uint8_t *webp = NULL;
    const size_t len = WebPEncodeRGBA(square, AVATAR_PX, AVATAR_PX, AVATAR_PX * 4, AVATAR_WEBP_QUALITY, &webp);
    if (len && webp) {
        out->webp = webp;
        out->webp_len = len;
        out->src_w = w;
        out->src_h = h;
        out->fmt = fmt;
        err = NULL;
    }
#endif
    free(square);
    return err;
}

const char *avatar_logo_normalize(const unsigned char *src, const size_t n, AvatarImage *out) {
    memset(out, 0, sizeof(*out));
    if (n > CLAN_LOGO_MAX_BYTES) return "FileTooLarge";
    const AvatarFormat fmt = avatar_sniff(src, n);
    if (fmt != AVATAR_FMT_PNG) return "UnsupportedType";
    if (!avatar_image_available()) return "AvatarsUnavailable";
    int w = 0, h = 0;
    if (!probe(fmt, src, n, &w, &h) || w <= 0 || h <= 0) return "BadImage";
    if (w < CLAN_LOGO_PX || h < CLAN_LOGO_PX) return "ImageTooSmall";
    if (w > CLAN_LOGO_PX || h > CLAN_LOGO_PX) return "ImageTooLarge";
    unsigned char *px = decode(fmt, src, n, w, h);
    if (!px) return "BadImage";
    const char *err = "BadImage";
#ifdef KN_HAVE_WEBP
    uint8_t *webp = NULL;
    const size_t len = WebPEncodeLosslessRGBA(px, w, h, w * 4, &webp);
    stbi_image_free(px);
    if (len && webp) {
        out->webp = webp;
        out->webp_len = len;
        out->src_w = w;
        out->src_h = h;
        out->fmt = fmt;
        err = NULL;
    }
#else
    stbi_image_free(px);
#endif
    return err;
}
