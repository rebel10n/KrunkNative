/*
 * webp.c - WebP decoding over simplewebp (client/lib/simplewebp, unmodified upstream, BSD-3-Clause):
 * the engine's only WebP path, so a .webp image no longer needs a .webp.png sidecar
 * (image.c keeps the sidecar as the fallback for animated WebP, which simplewebp does not decode).
 */
#include "webp.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wunused-parameter"
#pragma GCC diagnostic ignored "-Wsign-compare"
#pragma GCC diagnostic ignored "-Wshadow"
#pragma GCC diagnostic ignored "-Wconversion"
#pragma GCC diagnostic ignored "-Wimplicit-fallthrough"
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
#pragma GCC diagnostic ignored "-Wtype-limits"
#endif
#define SIMPLEWEBP_IMPLEMENTATION
#include "../../../lib/simplewebp/simplewebp.h"
#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

int webp_is_webp(const unsigned char *bytes, size_t len)
{
    return bytes && len >= 12 && !memcmp(bytes, "RIFF", 4) && !memcmp(bytes + 8, "WEBP", 4);
}

unsigned char *webp_decode_rgba(const unsigned char *bytes, size_t len, int *w, int *h)
{
    if (!webp_is_webp(bytes, len)) return NULL;
    simplewebp *img = NULL;
    /* simplewebp reads the buffer in place (not modified): the cast drops const for its API */
    if (simplewebp_load_from_memory((void *) bytes, len, NULL, &img) != SIMPLEWEBP_NO_ERROR || !img) return NULL;
    size_t iw = 0, ih = 0;
    simplewebp_get_dimensions(img, &iw, &ih);
    unsigned char *px = NULL;
    if (iw > 0 && ih > 0 && iw <= 16384 && ih <= 16384) {
        px = malloc(iw * ih * 4);
        if (px && simplewebp_decode(img, px, NULL) != SIMPLEWEBP_NO_ERROR) {
            free(px);
            px = NULL;
        }
    }
    simplewebp_unload(img);
    if (!px) return NULL;
    *w = (int) iw;
    *h = (int) ih;
    return px;
}
