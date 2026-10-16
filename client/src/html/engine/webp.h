/*
 * webp.h - WebP decoding for the image cache (image.c) over simplewebp (client/lib/simplewebp).
 */
#ifndef KN_HTML_WEBP_H
#define KN_HTML_WEBP_H

#include <stddef.h>

/* 1 when the bytes are a RIFF WebP container ("RIFF" <size> "WEBP"). */
int webp_is_webp(const unsigned char *bytes, size_t len);

/* Decodes a still WebP (lossy, lossy + alpha, lossless) to straight-alpha RGBA, malloc()ed (free()
 * it, or stbi_image_free(): both are free()). NULL for an animation or a decode error. */
unsigned char *webp_decode_rgba(const unsigned char *bytes, size_t len, int *w, int *h);

#endif
