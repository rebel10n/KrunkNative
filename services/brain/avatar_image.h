/*
 * services/brain/avatar_image.h - profile picture intake (avatar.c): sniff the format, check the
 * size and dimensions, decode, crop to a square, resample to AVATAR_PX and re-encode as WebP.
 * Only the re-encoded WebP leaves this module: the upload's bytes, its metadata and its own
 * dimensions never reach the database or a viewer. Pure C (no database, no network).
 *
 * Accepted: PNG, JPEG (stb_image) and WebP (libwebp, when the build found it; the output is WebP
 * too, so without libwebp avatars are unavailable). Everything else is refused by its first bytes,
 * never by its file name or Content-Type.
 */
#ifndef KN_BRAIN_AVATAR_IMAGE_H
#define KN_BRAIN_AVATAR_IMAGE_H

#include <stddef.h>
#include <stdint.h>

#define AVATAR_PX 256              /* the stored picture is AVATAR_PX x AVATAR_PX */
#define AVATAR_MIN_SRC_PX 32       /* smaller uploads are refused */
#define AVATAR_MAX_SRC_PX 2048     /* larger dimensions are refused (decoding is done on the loop thread) */
#define AVATAR_MAX_BYTES (512u * 1024u)  /* upload size cap, before decoding */

typedef enum { AVATAR_FMT_NONE = 0, AVATAR_FMT_PNG, AVATAR_FMT_JPEG, AVATAR_FMT_WEBP } AvatarFormat;

/* The format by the file's first bytes (PNG / JPEG / WebP), NONE for anything else. */
AvatarFormat avatar_sniff(const unsigned char *data, size_t n);

/* 1 when this build can decode WebP and encode the WebP output (libwebp found at configure time). */
int avatar_image_available(void);

typedef struct {
    uint8_t *webp;     /* the normalized picture (libwebp memory: avatar_image_free) */
    size_t webp_len;
    int src_w, src_h;  /* the upload's dimensions */
    AvatarFormat fmt;
} AvatarImage;

/* Normalizes an upload. NULL on success (out filled); else a short error name for the HTTP
 * reply: "UnsupportedType" (not PNG / JPEG / WebP), "FileTooLarge", "ImageTooSmall",
 * "ImageTooLarge" (dimensions), "BadImage" (does not decode), "AvatarsUnavailable". */
const char *avatar_image_normalize(const unsigned char *src, size_t n, AvatarImage *out);
void avatar_image_free(AvatarImage *img);

#endif
